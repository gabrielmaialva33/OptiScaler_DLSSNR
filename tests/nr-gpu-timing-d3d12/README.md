# Isolated D3D12 GPU timing integration

Run `python3 tests/nr-gpu-timing-d3d12/run.py` after reserving the MSVC/Wine build window.
`--build-only` and `--run-only` split compilation from execution. Output goes directly to
files under ignored `artifacts/`; no shell pipes and no solution build are involved.

This compiles the actual `DlssNr_GpuTiming.cpp` and `DlssNr_Submission.cpp` with a small
`ResTrack_Dx12` shim. The shim installs a real Detours ExecuteCommandLists hook and wraps
the real queue call in the production submission Batch. Reset is hooked by the production
submission code. Logger/pch shims remove unrelated DLL dependencies. A real GPU CopyResource
workload stands in for the model; no NGX, game, marker or installed DLL is loaded or changed.

The compiler prefix is an isolated copy of the local MSVC prefix. The runtime prefix is
created under this test's artifacts. Native D3D12 DLLs come from the installed
GE-Proton11-6-x86_64 vkd3d-proton runtime; the script records their hashes. Missing runtime,
compiler, hook admission, actual timestamps or coverage fails explicitly.

Mandatory cases: disabled timing creates no query heap/readback or submission hook;
normal recording publishes only after queue fence completion and Reset; replay on the
same queue and a second queue is rejected; Reset with the GPU blocked by a test fence
does not publish; releasing that gate permits publication. The test may poll/sleep on
its orchestration thread; production timing collection must not wait for the GPU.

Passing requires at least two accepted samples, both replay rejections, actual hooked
executions and allocations, positive timestamp durations, and a separate disabled-only
negative control that exits nonzero with ZERO COVERAGE. Source/binary hash mismatch
refuses a stale run. No result is valid while `result.json` is RUNNING or FAIL.

This does not prove production ResTrack installation, renderer marker placement, NGX
completion semantics, any game path or A/B performance. Real-game acceptance remains
separate. The harness is outside all shipping project/package inputs.
