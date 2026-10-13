# Submission-ordered retirement for opt-in D3D12 NR work

Design before implementation. The one-pass/default renderer is not enrolled.
Tracked recording epochs remain replayable until a successful graphics-list Reset.
Submission snapshots reserve a pending use before ExecuteCommandLists, and bind a
queue fence signaled AFTER that original call to the exact captured epoch. Reset
between submission and signal cannot transfer the fence to a newer recording.

A retired Usage is ready only when every epoch has been successfully reset, was
actually submitted, has no pending notification or unknown outcome, and every queue
fence reached its value. UINT64_MAX means device removal, never success. Reset alone
does not imply GPU completion, and completion alone does not prevent list replay.

Known Streamline wrappers use ResTrack's real-object convention, followed by COM
IUnknown identity normalization across interfaces. Admission requires one device
identity and the real Reset/Execute implementations in the same native d3d12.dll
or d3d12core.dll module. Other runtimes, replacement devices, unknown Reset
implementations, bundles and compute command lists are refused. This deliberately
narrow domain relies on compatible queues of that native runtime reaching its
hooked Execute implementation; foreign/custom queue implementations are outside
the contract, not silently supported. Missing submissions, failed
signals and discarded recordings with no submission retain their owners. Limits
are explicit: 256 epochs in the tracker, 64 epochs per Usage and 32 queues. A full
tracker refuses new recording rather than evicting potentially live resources.

The caller owns retirement storage and must bound it, refuse rebuilds at capacity,
and never clear an unresolved Usage while freeing its resources. There is no timer,
frame-count or 32-evaluation fallback. Keeping a resource alive is not an implicit
GPU ordering guarantee for reusing its contents; the renderer must independently
order dependent passes and reject unsupported concurrent recording.

The opt-in queue and Reset hooks remain installed after FG hooks are released or
NR is toggled off, because previously recorded lists may still replay. The initial
disabled/default path creates no fence, epoch or hook. Tracked submissions are
serialized across the original Execute and its Signal to prevent decreasing fence
values under concurrent CPU submits. Unrelated lists do not allocate a batch.

Host model tests establish the state machine, not actual Windows/Proton hook
coverage, driver behavior, NGX internal synchronization or real GPU validation.
Queue/list references are intentionally retained within bounded storage; no shared
device teardown may silently free unresolved NR objects.

Adversarial review: Reset cannot clear pending submissions; notification uses the
captured old epoch; repeated execute advances the fence requirement; all queues
must complete; failed Signal poisons the epoch permanently; UINT64_MAX fails; a
successful Reset without any observed submission never becomes ready; exhausted
admission leaves previous Usage entries intact. Both FG early-return and ordinary
Execute hook paths have an immediate post-original notification. No renderer
callback occurs under the tracker lock. GPU barriers and resource-content reuse
remain the renderer's responsibility.

API contracts: [ExecuteCommandLists](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-executecommandlists),
[queue Signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-signal),
[graphics-list Reset](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-reset),
[GetCompletedValue](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-getcompletedvalue).
