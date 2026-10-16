# Fork compatibility and GPU lifetime fixes (2026-10-16)

Four fixes from the fork audit, adapted to Retoque rather than merged wholesale. No NR model, shader,
colour law, feature default or deployment changes.

- `XeFGPath` and `XeLLPath` load into their own library settings instead of overwriting `XeSSPath`.
  Source: [janblade 241ae80e](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/241ae80e).
- NGX signature redirects recognize only the normalized basename `nvngx.dll`. The real `_nvngx.dll`,
  NR forwarder, similarly named files and matching directory names retain their own validation.
  Source: [A7mdwassa 4ba1811d](https://github.com/A7mdwassa/OptiScaler-DLSSNR-PreSR-Multipass/commit/4ba1811d).
- The D3D12 menu records a completion ticket before submission and signals its own fence afterward.
  Allocators wait before reset. Switching queues drains the prior overlay work; the submitted queue is
  retained until retirement. Cleanup waits for the last overlay ticket, including target-only cleanup,
  rather than fencing later FG work that may depend on a resize. A failed signal never certifies work.
  A live-device timeout keeps all resources; device loss permits release, not further rendering.
  Waiting polls for at most two seconds without registering a wait event whose handle could outlive cleanup.
  Public cleanup deactivates FG before taking the overlay mutex, since deactivation can join a present
  thread which needs that mutex. Reinitialization stops if the old resources cannot be retired.
  Sources: [upstream f062eee2](https://github.com/optiscaler/OptiScaler/commit/f062eee2),
  [janblade f9c3221d](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/f9c3221d).
- FG resize and release use a guard for the exact mutex acquired, independent of later configuration or
  current-FG changes. The existing FG instances live for the process (shutdown releases their resources,
  not the instance); changing that ownership requires revisiting these captured references. Dispatch
  no longer releases the swapchain-release owner's FSR mutex.
  Sources: [janblade ddde0feb](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/ddde0feb),
  [0f114fa3](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/0f114fa3),
  [0b6de533](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/commit/0b6de533).

## Validation boundaries

`fork-compat`, `fg-locks` and the extended `bridge-lifetime` suite compile the real changed code against
host fakes with ASan/UBSan. The complete host tier and the MSVC Release build are required checks.
These checks do not validate the D3D12 driver or prove an improvement in game performance.

Before integrating into the default branch: open/close the menu under DLSS-G and resize or change
fullscreen on a D3D11 bridge title, including native Windows. Check custom library paths and the NGX
bootstrap where signature spoofing is active. No game DLLs were deployed as part of this patch.
