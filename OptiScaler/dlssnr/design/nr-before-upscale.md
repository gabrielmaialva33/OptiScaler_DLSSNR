# Optional Neural Rendering before upscaling

Status: experimental port built and host boundary tests passed (2026-09-05).
No local in-game performance or image-quality claim. Not deployed.

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
strict fake resources under ASan/UBSan: 31 cases, 1000 allocation-failure attempts
(one allocation), 1000 changing-resolution evaluations (zero allocations), and 1000
same-frame gate checks (no evaluation allowed). The fake composition controls its
write result; it does not execute `Dispatch`, HLSL, NGX or a real upscaler.

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
