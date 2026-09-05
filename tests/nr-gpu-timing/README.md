# NR GPU timing host checks

Run from the repository root:

```sh
python3 tests/nr-gpu-timing/run.py
```

The runner builds with Clang ASan/UBSan and executes two binaries (the runtime binary also runs cadence cases in a fresh process). Nonzero exit, any sanitizer
finding, wrong case count or zero instrumented queries/maps fails the run loudly.

- Portable production certificate and timestamp decoding: 27 cases plus 3000 epoch cycles. These
  cover pending/failed Reset, completion before Reset, Reset before completion, device loss,
  replay, multiple queues, missing notification, unknown submission, zero frequency, never observed
  submission, incomplete/misordered/wrapped timestamps and explicit outside-model gaps.
- The production `DlssNr_GpuTiming.cpp` with fake D3D12 interfaces: disabled allocation/hook/query
  inactivity, no Map before both completion and Reset, one/three model passes, early aborts (including
  an unmatched model begin), replay/multiple queues, injected Map/Track/heap/readback failures,
  pending collection after disable, out-of-order completion without regressing the latest sample,
  immutable copied metadata, and bounded retention/pool refusal.

The fake GPU deliberately writes timestamps synchronously. Thus these tests prove host admission,
query accounting, immutable sample metadata, publication gating and retention decisions, **not** real
GPU timestamp execution, Detours interception, NGX runtime behavior, or game performance. Resources
with unknown submissions remain reachable intentionally; ASan must not be disabled to hide other leaks.
A fresh-process cadence run checks interval 30 (eligible evaluations 1 and 31 record, 2..30 do not),
no advancement while disabled, preserved phase after off/on, changed intervals and the recorded
interval remaining immutable while collection is pending. The phase is process-global over eligible
evaluations: off/on or changing the interval does not reset the counter.

Real D3D12 tests and the requested Crimson Desert/model A/B acceptance are separate evidence.
