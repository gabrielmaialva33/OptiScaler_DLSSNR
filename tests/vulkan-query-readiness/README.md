# Vulkan upscaler query readiness (host)

```bash
python3 tests/vulkan-query-readiness/run.py
python3 tests/run_all.py --only vulkan-query-readiness -v
```

The runner compiles the actual `UpscalerTime_Vk.h` class and every method in
`UpscalerTime_Vk.cpp`, removing only their platform includes and `#pragma once`.
The Vulkan API and `State` are local fakes. It builds with `g++`, C++20,
`-Wall -Wextra -Werror`, ASan and UBSan in a temporary directory. Each case runs
in a fresh process so the production class's private static state is not
replaced or manipulated by the test.

## Contract exercised

- No pool, no completed recording: no query poll or publication.
- `VK_NOT_READY` with plausible but unusable data: no history change.
- `VK_NOT_READY -> VK_SUCCESS`: retain the pending read and publish once.
  A separate zero-result case isolates premature consumption from publication.
- Repeated `VK_NOT_READY` with partial or no writes: retain the pending read.
- Query errors: discard without publication or retry; a new recording can arm
  another measurement.
- Success: apply `timestampPeriod`, keep the history size, consume once.
- Preserve the existing duration filter (zero, reversed sample, 5000 ms).
  A completed rejected duration is also consumed once.
- Re-recording uses the same pair in reset/start/end order. Poll flags stay
  exactly `VK_QUERY_RESULT_64_BIT`; no WAIT or PARTIAL flags are allowed.

The fake intentionally supplies plausible timestamp words with unsuccessful
results. This makes the original bug deterministic, without depending on
uninitialized stack contents or expecting ASan/UBSan to detect uninitialized
reads. Additional partial/no-write cases exercise the corrected status guard.

## Lifecycle and limits

`NVNGX_DLSS_Vk.cpp` initializes the pool, calls `UpscaleStart` before `Evaluate`,
and normally calls `UpscaleEnd` afterward. `UpscaleStart` records a reset of
the same two queries and the start timestamp; `UpscaleEnd` records the end and
arms a host boolean. The fallback branch after a failed/uninitialized
evaluation can return before `UpscaleEnd`. `hkvkQueuePresentKHR` polls at its
entry, **before** calling the real `vkQueuePresentKHR`, so that call's GPU wait
semaphores do not establish readiness for the host poll.

This fix only enforces the return-status contract. It does not establish
submission, GPU completion, synchronization, or association with a frame.
The pair is still reused on every start, and the boolean has no generation ID.
A retained pending read can overlap a new recording (including an aborted
evaluation); the older sample can be overwritten, or a poll can see older
available data before the newly recorded reset executes. The `record-again`
case deliberately asserts no frame identity.

The [Vulkan query-results specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)
explicitly describes this stale-availability hazard, including when WAIT is
used. Recording a reset is not executing a reset. The fake records commands
without pretending they immediately change GPU availability.

There is no new ring, fence, wait, or change to recording/call sites. The suite
does not validate a real Vulkan driver, GPU ordering, timestamp valid-bit/wrap
handling, multiple devices/threads, query-pool lifetime, or in-game statistics.

## RED / GREEN evidence

Before changing production code, the suite built successfully against the
original implementation. Five cases failed: `not-ready-retry`,
`not-ready-pending`, `partial-not-ready`, `errors`, and `record-again`.
`not-ready-pending` failed because the second poll never happened; the other
four caught publication from unsuccessful query results. The inactive,
success-once, and invalid-duration cases already passed.

With the status guard, all eight cases pass. This proves the host readiness
and consumption logic, not the lifecycle properties listed above.
