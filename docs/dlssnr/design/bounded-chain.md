# Bounded D3D12 model chain

Implementation review, 2026-09-05. Sources: PR #23 separate histories/admission and PR #26
sparse settings. This adapts their behavior to both current D3D12 NR stages and preserves
the original codec/proxy rather than importing either stale shader pipeline.

## Contract

- One to three passes, default one/master. Each extra pass owns its NGX feature/history.
  `modelInput` remains the untouched working-resolution proxy. Outputs alternate between
  `output` and a private working-resolution `passPing`; never write into `modelInput`.
- Only successful evaluations advance the selected answer. Later failure stops the chain,
  retires the failed instance, latches retry and composes the previous successful answer.
  All intermediate read states return to UAV. The final selected answer is transitioned
  for composition and restored; supersampling downsamples that answer exactly once.
- A locked snapshot resolves master/individual settings consistently. Changes settle for
  500 ms; working dimensions/format also settle for 500 ms. Only changed instances rebuild;
  upstream rebuilds reset downstream histories. Creation spacing is at least 500 ms and
  one creation per observed present. Evaluation is at most once per observed present.
- Every newly created feature additionally waits until a later present AND its recorded
  creation has actually been submitted and completed. Present advancement alone is not
  GPU completion. A missing/failed/unknown submission never satisfies this gate.
- Native Vulkan and driver proxy remain one/master. Opting in after any legacy untracked
  NR work requires restart; it cannot retroactively create lifetime evidence. Startup with
  one/master installs no new submission tracking and preserves original model arguments.
  Once activated, tracking stays active even if count returns to one.

## Resource evidence and limits

Before starting a NEW experimental recording, all preceding NR usage must be Ready:
successful Reset sealed the epochs against replay and every observed GPU fence completed.
This CPU admission gate prevents overlapping writes/history use across queues and upload
ring rewrites while an older NR recording could still execute. A lease explicitly owned by
`ScopedPreUpscale` may be borrowed by its inner Dispatch and remains alive through the
upscaler evaluation and scratch-restoration barrier; another scope cannot borrow just by
using the same command-list pointer. Scope exit releases only its own token, including early
returns and exceptions. Default one/master startup does not allocate this lease.

Before recording admitted NR work (including the pre-upscale scratch loan), the renderer
tracks that command-list epoch. Retired textures, features and replaced supersampling scaler
objects copy a conservative snapshot of all NR usage. They are freed only when each epoch
is sealed by successful Reset and every observed submission fence is complete. Completed
but replayable lists are not safe to release. Shutdown retains all objects if any current
or retired tracked usage is unresolved; it does not fall back to evaluation-count retirement.

A retirement admission cap refuses work before a recording/allocation when 32 parked
objects remain. One admitted recording can add its bounded cleanup batch before the next
admission check. This prevents repeated slider/DRS changes on an unsubmitted epoch from
creating unlimited retired generations. Unknown or missing completion retains memory and
can stop NR until recovery/restart; retention is intentional, not successful reclamation.

Adapter-local budget and current usage are queried before opted-in generation replacement
and each additional model. Admission reserves max(512 MiB, 20% budget), 150% of the measured
base-model usage delta (minimum estimate 256 MiB), and conservative texture storage.
These are measured-budget heuristics, not a guarantee: NGX allocations are opaque, allocations
may become visible asynchronously, and other processes can consume VRAM after admission.
Allocation/create/evaluation failures latch instead of repeatedly attempting the same work.

Waiting for Reset/fences skips NR rather than blocking the game. The model may therefore
run less frequently and throughput may decrease. Stage coverage logs must distinguish
those skips from model execution; a faster average caused by skipped frames is not a
model optimization. This is conservative serialization, not a high-throughput resource-ring
implementation. The admission decision and resulting refusal are INFO-visible.

The inherited one/master startup path still uses 32 evaluations for retirement; this new
opt-in does not silently change that default. Existing descriptor/constant-buffer rings,
meter/capture readbacks and GPU timer readbacks have their existing reuse/read policies.
The new experimental Ready gate prevents reuse across incomplete/replayable NR recordings;
it does not claim a runtime GPU validation of every readback policy or that a changed device
can safely reuse the module's existing objects. No in-game DRS stress
or general device-recreation safety is claimed by this change. Restart on device replacement.

## Adversarial review and evidence

Checked: one-pass/master evaluate call remains literal and passes the baseline verifier;
shader constants/HLSL/both compiled headers untouched; config comes from the four-point
snapshot owned by Config; original proxy never becomes an output; pass 2 failure keeps
pass 1 output, pass 3 failure keeps ping; odd/even final answer used by the only down-leg;
separate histories, settings debounce and downstream reset; fail latch prevents creation
storms; unknown submission cannot age into release; shutdown cannot release unresolved
tracked objects; retirement cap precedes new work; invalid pre-upscale exposure is cleared
before its outer transition scope, matching the inner texture contract.

`tests/nr-multipass` passes production portable routing/scheduling/admission helpers and
submission model under ASan/UBSan. It does not execute actual NGX/D3D12 or prove rendered
quality, GPU performance, real driver submission coverage or synchronization correctness.
Per-pass INFO cost is explicitly CPU-call time. Existing NGX GPU timing spans the full chain.

A diagnostic-only INFO spatial-contract report records Color allocation dimensions, the
render-subrect values and individual NGX query success/absence, game output, requested
model dimensions and WorkingScale. Identical contracts do not log again. A before-stage
model at least as large as game output says `no spatial reduction`, without inferring DLAA
from allocation size. The boundary suite executes that reporter and checks query provenance,
no repeat for an unchanged contract and no parameter/resource writes from diagnostics.
