# Vulkan GPU timer query readiness (host)

```bash
python3 tests/vulkan-query-readiness/run.py
python3 tests/run_all.py --only vulkan-query-readiness -v
```

The runner compiles the actual `GpuTime_Vk.h` classes (`GpuTime_Vk`, `ScopedGpuTime_Vk`) and every
method in `GpuTime_Vk.cpp`, removing only their platform includes and `#pragma once`. The Vulkan API,
`State` and `LOG_ERROR` are local fakes. It builds with `g++`, C++20, `-Wall -Wextra -Werror`, ASan and
UBSan in a temporary directory, and runs each case in a fresh process.

## Subject, and why it moved

Until the 2026-10-12 merge of `optiscaler/master`, this suite held `upscaler_time/UpscalerTime_Vk` to
our fix `05e0adb8`: publish a Vulkan upscaler time only when `vkGetQueryPoolResults` returns
`VK_SUCCESS`, because a `VK_NOT_READY` read hands back plausible garbage. Upstream `e199fac4` deleted
that class and replaced it with `gpu_time/GpuTime_Vk`, which every `Shader_Vk` now owns — the upscaler
features, RCAS, Output Scaling, the menu blur, and Neural Rendering's `DlssNr_Vk` and its
supersample legs. `GpuTime_Vk::ReadGpuTime` carries the same status guard, so the suite now holds that
class to it.

## Contract exercised

- No device, or a failed `vkCreateQueryPool`: nothing recorded, nothing polled, nothing destroyed.
  `ScopedGpuTime_Vk` with no timer or no command buffer does nothing.
- Start without End never arms a read.
- Three start/end pairs in one pool. Start advances the ring and records a reset of its pair and a
  top-of-pipe timestamp; End records the bottom-of-pipe timestamp and arms the pair. A read targets
  the oldest pair, so a recording is read two recordings later.
- `VK_NOT_READY`, with plausible, partial or no writes: no publication, and the same pair is polled
  again until it succeeds; a published pair is consumed.
- Query errors: no publication. The pair stays armed and is read again until it succeeds or the ring
  records over it (upstream's behaviour; the deleted class consumed an error once).
- A completed pair whose end precedes its start publishes nothing and is consumed.
- Success applies `timestampPeriod`. Poll flags are exactly `VK_QUERY_RESULT_64_BIT`: no WAIT or
  PARTIAL.
- The pool is destroyed with its timer, once, and abandoned at process shutdown.

Not filtered here, unlike the deleted class: a zero duration, and implausibly long ones. The caller
filters (`hkvkQueuePresentKHR` drops upscaler times of 100 ms and more).

## RED / GREEN evidence

With the `result != VK_SUCCESS` return removed from `ReadGpuTime` (a scratch copy, 2026-10-12),
`not-ready-retry` and `errors` fail on their first publication check; with it, all seven cases pass.

## Limits

This proves the host readiness and consumption logic only. It does not establish submission, GPU
completion, synchronization or frame identity: a pair is reused every three recordings, and a
retained read can overlap a new recording of the same pair (the
[Vulkan query-results specification](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)
describes this stale-availability hazard). It does not validate a real driver, timestamp valid bits or
wrap, multiple threads, or in-game statistics.
