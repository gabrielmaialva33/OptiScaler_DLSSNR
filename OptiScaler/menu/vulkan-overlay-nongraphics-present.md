# Vulkan overlay on a non-graphics present family

Date: 2026-09-11. Examined base: `da0d1901`, with the atlas flush change in
`f72490b9`. Scope: the native Vulkan overlay, not Neural Rendering.

## Decision

**Keep the non-graphics bailout. Do not enable a second-queue renderer yet.**
There is no defensible queue to borrow from the current `_deviceQueueCounts`
contract: all entries describe application-created queues, and none conveys
exclusive access or a shared host lock. The hardware has spare capacity in the
harness, but capacity is not permission to submit concurrently on an existing
application queue.

A queue reserved by this layer at device creation is a possible architectural
solution, not something the current recorder already provides. An ordinary
reserved queue still needs a complete device-wide idle/destruction synchronization
contract. The alternative of a separately reserved, internally synchronized queue
was tested under the installed Wine runtime and produced an unexpected validation
error during device creation. Neither route currently supplies the complete,
validated ownership contract needed to authorize cross-family image writes.

This is a stop at the requested host-synchronization gate, not a claim that the
feature is impossible. No production drawing implementation or inversion of the
bailout tests accompanies this note. The work required to reopen that gate is
explicit below.

## What the current code knows

`hooks/Vulkan_Hooks.cpp::hkvkCreateDevice` records queue counts **after** the real
`vkCreateDevice` succeeds. `menu_overlay_vk.cpp::NoteDeviceQueues` stores the maximum
count for each family, keyed by `VkDevice`. It does not retain queue-create flags,
priorities, feature enables, a reservation owner, or the application's submission
mutex. Records are not removed on device destruction. Multiple queue-create entries
with different flags cannot be distinguished by that map.

`CreateVulkanObjects` selects a graphics family and retrieves its queue zero. It
also enumerates the queues represented by the counts into `_queueFamilyOfQueue`.
`QueuePresent` can therefore identify the presenting queue's family. The two menu
mutexes protect our ImGui and swapchain state; the game does not take those mutexes.

The normal rendered-frame submit is on the queue whose `vkQueuePresentKHR` call
we intercepted. For a conforming game, its host synchronization for that queue
covers the intercepted call, including our nested submission. That argument does
not extend to a different graphics queue used by another game thread. A semaphore
orders GPU work, not the two threads entering a queue API. The relevant rule is
[host synchronization for vkQueueSubmit](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).

There is also an existing boundary to audit independently: initialization and ImGui
texture uploads use `_ImVulkan_Info.Queue`, while creation, teardown and the backend
contain device-wide drains. A serialized harness does not prove those operations
safe in an arbitrary concurrent game. This note does not turn the presenting-queue
argument into a blanket guarantee about every current overlay operation.

## Queue acquisition: rejected shortcuts and possible routes

| Candidate | Decision and reason |
|---|---|
| Graphics queue zero, or the last recorded graphics queue | Reject. Both may be in use by the application. |
| A created queue the game has not retrieved yet | Reject. The game may retrieve and use it later. Observation is not a reservation. |
| An index below the hardware queue count but above the created count | Reject. No such queue was created on the device. `vkGetDeviceQueue` retrieves; it does not create. |
| Hold `_vkPresentMutex` around a submit to the game's graphics queue | Reject. This does not synchronize with the game's graphics thread. |
| Wait for the graphics queue/device to idle, then submit | Reject as a locking scheme. The wait itself needs host synchronization; a new game submission can race immediately afterward. |
| Use another logical device | Reject for this change. A swapchain image handle is owned by its logical device; this would require a different external-memory/composition design. |
| Reserve an additional ordinary graphics queue at device creation | Potential route, with the ownership/lifetime work below. Not available from the existing counts after creation. |
| Reserve an internally synchronized graphics queue | Potential route in the specification; the installed Wine path did not pass the runtime probe described below. |

The created-index and queue-flag requirements are specified by
[vkGetDeviceQueue](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDeviceQueue.html).

### What an ordinary reservation would have to establish

Before calling the real device creation function, preserve the application's queue
requests, find spare graphics capacity after accounting for **all** requested
queues/flag classes, and reserve an additional index exclusively for the overlay.
Keep the original request count and the reserved handle in separate per-device
records. Never infer a reservation later from a count or retrieve an index that
was not actually added. Devices created before interception, exhausted families,
unsupported protected/device-group/priority combinations, or unsuccessful optional
creation must retain the original application request and the bailout. A failed
optional creation cannot make the game lose an otherwise valid device.

That is enough to avoid borrowing the application's graphics submission queue; it
is **not** the whole host contract. `vkDeviceWaitIdle` requires synchronization of
all non-internally-synchronized queues of that device, including a hidden reserved
queue. See [vkDeviceWaitIdle](https://docs.vulkan.org/refpages/latest/refpages/source/vkDeviceWaitIdle.html).

A future reservation owner must cover every access to its queue, including backend
font/texture uploads and waits, and coordinate with application device-wide waits,
destruction and unhooking. One possible boundary is to confine all added host queue
operations to the intercepted present's lifetime: a correctly synchronized game
cannot concurrently call a device-wide idle on the presenting queue. This argument
must be made true for initialization, delayed uploads and cleanup too; the current
backend submits and drains outside that boundary. It cannot simply be assumed.
Another approach is a device-lifetime service that interposes all relevant idle and
destruction dispatch paths and coordinates with the reservation mutex. Its lock
ordering must prevent recursion and deadlocks with the present/cleanup mutexes.

Today's `HookDevice` installs swapchain creation and presentation hooks, not such
a device-lifetime service. The general proc-address wrappers do not provide a
common lock for all submit/submit2/KHR alias, bind-sparse, wait-idle and destroy
paths. The separate Vulkan-to-DX12 bridge's submission hooks are not a global native
Vulkan host-synchronization contract. A partial set of hooks is insufficient.

### Internally synchronized queue experiment

[VK_KHR_internally_synchronized_queues](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_internally_synchronized_queues.html)
provides an opt-in queue-create flag backed by a feature enable. It must be arranged
at device creation; it cannot retrofit a live queue. A separate flagged queue would
avoid changing the game's ordinary queue flags and remove the external host-lock
requirement for that reserved queue. It would still need correct resource lifetime
and GPU synchronization.

Live checks on RTX 4090 / driver 615.71.09 found the extension and feature both in
native `vulkaninfo` and through Wine (physical device API 1.4.351). A standalone
probe linked against the repository Vulkan import library and loaded the same
instrumented OptiScaler DLL and native validation environment as the suite.
It compared these requests on family zero:

- Ordinary control: two ordinary queues. Distinct handles, successful empty
  submission and queue wait, zero validation errors, process exit 0.
- Candidate: one ordinary queue plus one queue with
  `VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR` (`0x4`), extension and
  feature enabled. Native validation reported `VUID-vkGetDeviceQueue-flags-01841`
  **inside `vkCreateDevice`, before the probe's explicit queue retrieval**. The
  final candidate run obtained distinct handles and submitted successfully, but
  still had one validation error and exited 9. An earlier candidate attempt did
  not obtain distinct handles. Success of a submit does not excuse that error.

The decisive final comparison is preserved here so it survives cleaning the ignored
artifacts. `0` for create/submit/wait is `VK_SUCCESS`; `distinct queues=1` means two
non-null, different handles. Both requests used family 0 on the same GPU/runtime:

| Measurement | Ordinary control | Internally synchronized candidate |
|---|---:|---:|
| Wine-visible physical-device API | 1.4.351 | 1.4.351 |
| Extension exposed / feature value | yes / 1 | yes / 1 |
| Queue requests (count, flags) | (2, 0) | (1, 0) + (1, 0x4) |
| `vkCreateDevice` result | 0 | 0 |
| `distinct queues` | 1 | 1 |
| `vkQueueSubmit` / `vkQueueWaitIdle` result | 0 / 0 | 0 / 0 |
| Unexpected validation errors | 0 | 1 |
| Process exit | 0 | 9 |

The candidate's decisive diagnostic occurred between the probe's
`BEGIN vkCreateDevice` and `internal device create=0` messages, before
`BEGIN application vkGetDeviceQueue`:

```text
VALIDATION ERROR: vkGetDeviceQueue(): queueFamilyIndex (0) was created with a non-zero VkDeviceQueueCreateFlags in vkCreateDevice::pCreateInfo->pQueueCreateInfos[1]. Need to use vkGetDeviceQueue2 instead.
VUID-vkGetDeviceQueue-flags-01841
```

This localizes a problem below the application's explicit retrieval in this runtime
path; it does not establish which Wine/loader component is defective. No production
workaround or validation exception is justified by this experiment. The repository's
pinned Vulkan headers also predate this extension; the disposable probe used the
upstream ABI declarations, without changing those headers.

Evidence: `tests/vulkan-overlay/artifacts/queue-capability-probe.cpp`,
`queue-capability-ordinary.log`, and `queue-capability-internal.log`. Earlier bare
probes additionally hit an extension-dependency diagnostic; the comparison above
loads the normal DLL and removes that confounder. These probes are diagnostic
artifacts, not added suites and not substitutes for concurrent-host tests.

## GPU contract once a safe graphics queue exists

Let `P` be the actual presenting/owning family, `G` the graphics writer's family,
and `W` the **entire** incoming present semaphore list. First restrict support to
one tracked swapchain/image, known queue ownership, unprotected queues and a
supported present chain. Reject unsupported cases before consuming `W`.
The graphics queue must satisfy the host contract above independently of this GPU
protocol. All command pools must belong to their submission families.

### EXCLUSIVE: two pairs of barriers, three submissions

At interception the image must already be valid for presentation on `P`, including
its incoming layout and ownership. Use the following proposed sequence:

| Submission | Queue | Wait | Recorded work | Signal |
|---|---|---|---|---|
| 1 | P | All W | Release image ownership P to G | released_to_graphics |
| 2 | G | released_to_graphics | Acquire P to G; render overlay; release G to P | released_to_present |
| 3 | P | released_to_present | Acquire G to P | overlay_finished |
| Real present | P | overlay_finished | Original swapchain/image presentation | WSI completion |

For each pair, release and acquire must specify identical image/subresource range,
source/destination family indices, and old/new layouts. The P-to-G pair uses
`srcQueueFamilyIndex=P`, `dstQueueFamilyIndex=G`; the reverse pair uses `G,P`.
Do not use `VK_QUEUE_FAMILY_IGNORED` for either pair.

A conservative initial design preserves `PRESENT_SRC_KHR` on both sides of each
ownership pair, leaving the existing render pass to transition into and out of
`COLOR_ATTACHMENT_OPTIMAL`. Each acquire must follow its matching release through
the semaphore dependency. Begin with `ALL_COMMANDS` waits for ownership transfers,
then narrow only with evidence. Barrier stage/access masks must be valid on their
queue: the P command buffers cannot use graphics-only pipeline stages. The graphics
barriers and render-pass dependencies must cover attachment reads/writes and the
final layout transition. These requirements follow the
[queue-family ownership transfer rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers).

A release alone does not transfer usable ownership to the writer, and an acquire
alone cannot substitute for the owner's release. Both directions are required.
All original binary wait semaphores are consumed exactly once, by submission 1;
the real present must not wait on them again.

### CONCURRENT with both families listed

Require `_scSharingMode == VK_SHARING_MODE_CONCURRENT` and membership of **both** P
and G in `_scSharedFamilies`. Submit the overlay on the safe graphics queue, wait
on all W, signal `overlay_finished`, and make real present wait on that semaphore.
No queue-family ownership barriers are needed. Normal image layout and render-pass
dependencies still are.

If G is absent from the family list, bail out. Contrary to the current diagnostic's
wording for that case, an ownership transfer cannot expand a CONCURRENT image's
creation-time family list. Access from an unlisted family violates
[VUID-vkQueueSubmit-pSubmits-04626](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).
Changing the application's swapchain sharing contract is outside this proposal.

### Lifetime

Preallocate per-image command buffers for both families, both intermediate
semaphores, the final present semaphore, and completion tracking. Reuse only after
proof that the entire relevant submission chain has finished. The final present
semaphore also needs proof of consumption by WSI; a graphics fence alone does not
provide that proof. Keep it associated with the acquired image or use supported
present-completion facilities. Real present's host-visible wait-list storage must
survive the hook returning, as the current thread-local handoff already requires.
See [vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).

## Failure must preserve the original presentation

The safe fallback boundary is **before the first successful submission consumes
W or releases ownership**. Resolve the queue capability and sharing eligibility,
allocate every resource, finish every command buffer, prepare all submission
structures and recovery state, and check fence/resource availability before that
boundary. Any failure there calls `ImGui::Render` exactly once and keeps the
original present and semaphore list intact: today's honest bailout.

After submission 1 succeeds, simply returning true is wrong: the original waits
may be consumed and ownership may be released. Preallocation cannot guarantee
that later `vkQueueSubmit` calls will succeed. A future implementation needs an
explicit state machine for partial chain submission and a pre-recorded no-draw
restore path which completes the pending acquire and reverse pair before allowing
presentation. It must never queue a wait for a signal whose submit failed.
Submission out-of-memory guarantees concern the failing call, not a rollback of
earlier successful calls; see [vkQueueSubmit failure semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).

Recovery can itself fail. The implementation must report the real Vulkan failure
and stop submitting/presenting that invalid chain, rather than claim an ordinary
no-overlay success or manufacture an unsignaled wait. No software design can promise
that real device loss/OOM is impossible. The requested no-risk setup fallback can
be guaranteed before committing the chain; it cannot honestly be promised as an
unconditional rollback afterward. This is a second implementation gate.

## Work required before enabling drawing

1. Establish a queue reservation/lifetime contract. Either fix and validate the
   internally synchronized runtime route, or implement a complete ordinary-private-
   queue host/lifetime boundary. Test actual concurrent game submissions, idle,
   recreation, destruction, cached proc addresses and multiple devices. A single
   successful empty submit does not prove this contract.
2. Implement and fault-test the GPU chain and its partial-submit recovery. Cover
   every setup failure and submit boundary without suppressing validation errors.
3. Add positive controls for CONCURRENT and EXCLUSIVE: at least one actual overlay
   submission, zero non-graphics bailouts, correct queue ownership and zero unexpected
   validation errors, including synchronization hazards. Retain separate fallback
   controls for no reserved queue, unknown queue, unsupported family list and injected
   setup failure. The EXCLUSIVE harness must continue clearing on P alone.
4. Run allocation/object accounting, repeated recreation, delayed GPU completion,
   teardown and concurrency tests, followed by DOOM Eternal acceptance testing.

Until step 1 has a defensible tested answer, task 3 is intentionally not implemented.
The existing EXCLUSIVE and CONCURRENT bailout assertions remain accurate and intact.
