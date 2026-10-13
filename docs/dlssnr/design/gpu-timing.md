# Fence-confirmed NR GPU timing

## Contract (written before implementation)

Timing is optional and defaults off. It must neither enable multipass nor change render admission.
No query heap, readback allocation or submission hook is created by the disabled path. The renderer
passes an immutable evaluation contract (stage, dimensions, settings generation, pass count) and the
configured sampling interval. Sampling counts eligible NR evaluations, not presentation frames.

A bounded pool owns one timestamp query heap and readback buffer per in-flight sample. Each sample
records its total begin/end and up to three model begin/end pairs on the same command list, within one
renderer evaluation. ResolveQueryData only resolves initialized queries. Total, model sum and the
remaining measured interval come from that single sample; failed or incomplete evaluations are not
published as successful NR costs. The remaining interval includes all commands between total markers
outside model intervals, not a claim of isolated shader execution or pure CPU-independent overhead.

Submission tracking captures the exact recording epoch before ExecuteCommandLists and queues a fence
Signal afterward. A sample is publishable only after successful Reset seals that epoch against replay,
exactly one execution was observed on exactly one supported queue, and its signal completed. Timestamp
frequency comes from that actual queue. Duplicate lists in one Execute call and later replay invalidate
the sample. UINT64_MAX (device lost), missing notifications, failed Signal, unsupported runtime or queue,
failed timestamp frequency, failed Map and malformed timestamps never become estimated timing numbers.

Reading waits non-blockingly for both Reset and completion: this avoids a completed readback being
rewritten by a later replay. Discard is distinct from release. Unknown or unsealed GPU use retains the
slot; the bounded pool drops new samples under pressure instead of releasing resources still in use.
Disabling stops admission and polls retained slots; it does not stall or destroy pending GPU resources.

## Evidence and limitations

Host tests must exercise the production portable certificate and timestamp validation logic, fail when
zero cases execute, and cover replay, multi-queue execution, incomplete fence, device loss, missing
Signal, pending notification, Reset-before-completion and malformed timestamp intervals. They establish
host decision behavior, not D3D12/NGX correctness. Release compilation and adversarial review are also
required. No game installation is authorized by this task; Crimson Desert and SF-v2 versus rtx40 remain
external acceptance steps until the user installs and runs the instrumented build.

Microsoft contracts used: queue Signal completes after earlier queue work; GetCompletedValue returns
UINT64_MAX after device removal; GetTimestampFrequency belongs to the command queue;
ResolveQueryData requires initialized/completed queries and writes UINT64 timestamps to a COPY_DEST
buffer. References will be recorded alongside implementation verification.

## Implementation details and observation units

The renderer opens the scope before its first Dispatch resource barrier and declares it before its
restoration guards. Deferred successful finish closes after those guards. Early returns still close
and resolve only initialized timestamps, but the sample is discarded. `dispatch_ms` includes commands
recorded within this scope (including creation if it occurs there); surrounding pre-upscale loans,
the game's upscaler and other command-list work remain outside. `model_ms` sums up to three explicit
NGX evaluation brackets; `outside_model_ms` sums their adjacent gaps. These are elapsed GPU timestamp
intervals: scheduling/interference can affect them, so they are not a claim of isolated hardware cost.

`render` identifies the guide/render dimensions supplied by the renderer, `compose` the NR output
target (which can be a smaller scratch target before upscale), and `model` the working dimensions.
The structure keeps the historical API field name `outputWidth/Height` for `compose`; this is not
necessarily the final DLSS/game output. Per-sample contract hash, generation, stage, present ID,
evaluation ID, model identity, reset/capture flags and sampling interval survive asynchronous collection.

Sampling interval is 1..10000 eligible NR evaluations, default 30 at the configuration layer. It is
not a frame counter: several evaluations can share one present ID. The first eligible evaluation is
sampled. INFO emits every accepted sample with `proof=sealed-single-execution-fence`. Counts distinguish
eligible evaluations, admission attempts, recorded samples, accepted samples, discarded attempts or
samples, and retained/pending slots. Drop counts accumulate per reason; repeated log messages are
throttled to five seconds. Pending summaries distinguish Reset, GPU and notification waits. Missing
submissions, including a sealed recording with no observed execution, never authorize release.

The last accepted menu sample is explicitly historical: it carries a monotonic sample ID, age and
original interval/contract. Disabling retains historical values but marks timing disabled, releases
safe idle allocations and keeps unsafe pending slots. Lock order is timing state then submission
state; submission hooks never call the timing collector or renderer.

## Primary API references

- [Queue timestamp frequency](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-gettimestampfrequency)
- [ResolveQueryData and initialized-query requirement](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-resolvequerydata)
- [D3D12 timing semantics](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)
- [Queue Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal)
- [Fence completion/device removal](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue)

## Configuration provenance limit

An immutable Metadata copy protects the asynchronous collector from later changes. It does not prove
that every legacy renderer setting was read atomically from that same snapshot: existing rendering
code still reads some configuration fields directly. Use fixed configuration for each A/B measurement
window, restart between model binaries, exclude reset/capture windows, and record the DLL/model
identity externally as described in `tests/nr-timing-boundary/README.md`. A contract hash identifies
the recorded diagnostic configuration; it is not a proof that concurrent UI writes could not affect
other renderer reads. Sampling phase is global over eligible evaluations and persists across off/on
and interval changes. The latest displayed sample is the highest recorded sample ID that has been
accepted, not whichever fence happened to finish last. Its age starts at CPU recording, and a separate
collection age is available.
