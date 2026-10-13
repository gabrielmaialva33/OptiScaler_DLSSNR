# Present NR — a contract for clean content, without a present-thread wait

Status: **design for review, not implemented**. Read against `1ee7b758`, 2026-09-14.
This resolves the admission question left between `nr-present-hook.md:224-234` and `:541-546`.
It does not authorise a generic game present host. No production code, configuration, shader,
test, or installed game changes accompany this note; no new GPU measurements are claimed.

## Decision

**A signature of our output is a useful rejection witness, but cannot certify a clean input.**
Even exact equality testing cannot distinguish a new game image from a small modification of an
already enhanced image. Moving that test to the GPU removes a readback dependency, not the missing
information. The proposed signature gate is therefore rejected as the sole admission contract.

The first admissible target is a **cooperating, audited full-frame producer**: it identifies an
immutable clean source revision and the actual full-frame write that materialises that revision
in the selected backbuffer. The existing controlled harness can provide this evidence. **No game or
emulator has been verified to provide it yet.** A game-specific adapter is additional work, not
something `ResTrack_dx12` or the presence of our swapchain wrapper already supplies.

For arbitrary games, no cheap and correct contract has been established under the requested
constraints. That is a limit of the candidates examined here, not a claim that all future designs
are impossible. Source isolation or producer cooperation could change the answer.

Frame generation is refused, including native FG. An unknown FG state is unsupported; `FGInput` and
`FGOutput` resolving to `NoFG` alone do not establish that native FG is off. No generated/base-frame
inference, timeout stand-down, or FG synchronisation design belongs to this slice.

## What must be distinguished

Let `C` be a clean image made by the application, `E(C)` our enhanced result, and `B_i` the physical
backbuffer in slot `i`. Keep three different identities:

- **Allocation generation:** selected swapchain, canonical device/queue, resource identities,
  extent, format and colour space. An index or HWND alone is not this identity.
- **Clean source revision:** a producer-owned immutable image revision, independent of which slot
  receives it. Consecutive presentations of that revision do not require another model evaluation.
- **Materialisation:** a particular submitted full-frame write of that source revision into `B_i`.
  Rendering the same clean picture into the next slot is a new materialisation, not necessarily a
  new source revision. Re-entering Present without another write is neither.

Freshness is about **provenance**, not visual novelty. A full clean redraw of an unchanged scene is
safe to enhance; an altered HUD over `E(C)` is different but not clean. Suppressing repeated clean
revisions is a separate deduplication requirement. These two requirements must not be conflated.

Current evidence is insufficient for either producer identity:

| Existing item | What it proves | What it does not prove |
|---|---|---|
| `wrapped/wrapped_swapchain.cpp:524-525` | A non-test call passed that point; the counter increments before the original Present result at `:532-534` | A full rewrite, successful flip, or new clean source |
| `wrapped/wrapped_swapchain.cpp:1223-1227` | Which physical buffer is current | Which picture it contains or who last wrote it |
| `dlssnr/DlssNr_SubmissionModel.h:15-25,33-51` | Recorded list identity, executions, pending/unknown status, completion, and sealing against replay | Texture write coverage, the source of its pixels, or an image revision |
| `dlssnr/DlssNr_Submission.h:12-28` | Resource-use tracking and submission/timing boundaries | A producer ticket or detection of full backbuffer replacement |
| `tests/dlssnr-loopback/present_nr.h:452-464,523-526` | The harness uploads its own full fixture before NR and verifies exact source bytes afterwards | A contract supplied by a game |

`FLIP_DISCARD` is not a runtime witness either. Microsoft distinguishes full-redraw/discard and
partial/preserved usage; discard does not report that a clean redraw occurred. The Windows compositor
can also modify discard buffers. That last behaviour is not claimed as an observation under Proton.
See [DXGI 1.4 changes](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-4-improvements)
and [swap effects](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect).

## Attack on self-witnessing

Suppose we retain `H(E(C))` per allocation/slot and admit when `H(B_i)` differs.

1. **Unchanged output:** `B_i = E(C)`. Equality rejects it. This is the case the idea solves.
2. **Partial rewrite:** the game changes one counter pixel on `E(C)`. The signatures differ and
   the rule admits `E(C)` everywhere else for a second enhancement. No collision is involved.
3. **Our overlay:** NR writes `E(C)`, then our menu draws over it. On reuse, even our own subsequent
   work can defeat the witness. The current overlay call is `wrapped_swapchain.cpp:496`.
4. **Output moved or restored:** an older enhanced image, or one copied from another slot, need not
   match the most recent witness for `i`. Per-slot history alone misses it.
5. **Clean repeat:** the game writes `C` again, including into another slot. Usually `C != E(C)`;
   an output witness does not suppress this duplicate source evaluation. It would also need input
   identity/history, with explicit retention limits.

An indistinguishability example is decisive. In execution A, the game independently generates clean
image `X`. In execution B, it leaves most of `E(C)` intact and changes a small region so the resulting
image is also `X`. A present-only observer sees identical bytes, the same output witness, and the
same slot in both executions. Correct admission differs. No hash, exact image comparison, histogram,
or elapsed-present count recovers the missing provenance.

The collision argument is correct **only for a rejection-only rule**: a collision can reject a
clean image unnecessarily, losing an enhancement rather than applying another one. It does not make
the rule's non-equality branch safe. A corrupted/stale witness or generation mix-up is not a hash
collision and can cause false admission. Missing evidence must mean unknown, not fresh.

Do not use hash equality to substitute a cached output without an exact/producer identity check:
there, a collision can display the wrong picture. A sampled signature also misses small changes.
Neither may be advertised as byte-exact identity. A per-pixel/region mask does not solve provenance:
unchanged clean pixels can equal enhanced pixels, and changed pixels can still be derived from them.
Feeding a mixed image into the full-frame model is not repaired by masking only its final write-back.

## Alternatives and what they actually buy

### GPU comparison, predication, and indirect dispatch

A compute pass can compare the current input with a retained witness, reduce to an integer, insert
the required UAV ordering/state transition, and use an aligned 64-bit predicate. This avoids a CPU
readback for that decision. D3D12 predication supports Dispatch and CopyResource, amongst other
commands; query operations are not predicated. The predicate is snapped when SetPredication executes,
so its producer must precede it. Barriers still need a valid state path in both outcomes.
[Microsoft predication documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication).

This is suitable for GPU-side rejection of known pixels, **not proof of renewal**. Nor can we wrap
the current NR entry point in a predicate and claim the feature was skipped:

- `shaders/dlssnr/DlssNr_Dx12.cpp:1971,2005` takes a CPU mutex and records lifetime state before
  inference. Feature creation updates CPU state and returns false after recording setup at
  `:2584-2603`. `g_nr.reset` is cleared at `:3202` after the evaluation call.
- `dlssnr/forwarder/dlssnr_forwarder.cpp:897-948` writes CPU NGX parameters and calls the opaque
  model implementation. There is no GPU predicate or conditional-evaluation contract in this export.
- We have not verified whether the model preserves predication, changes it, records additional
  lists, or has CPU state that assumes its commands executed. Predicate inheritance into another
  direct list cannot be assumed: direct lists start with predication disabled.
  [SetPredication API](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication).

`ExecuteIndirect` consumes GPU arguments for supported graphics/compute commands and bindings. It
cannot conditionally invoke the CPU NGX export or automatically turn its opaque command sequence
into an indirect command signature. It could drive our own comparator/compositor, not substitute
for an audited conditional model API.
[Indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing).

Running NGX unconditionally and predicating only our copy-back can suppress a visible write, but
still pays inference and may advance model/CPU history on rejected content. That is not a skipped
evaluation. It cannot satisfy the proposed acceptance test or the model-work objective. Even an
eventual proven conditional NGX path would still need a positive clean-source contract.

### Delayed readback using buffer rotation

At visit `t` to slot `i`, compare `B_i(t)` with the retained output and submit the result for later
CPU inspection. At visit `t+N`, that result describes `B_i(t)`, not `B_i(t+N)`: the intervening
producer write is exactly the event we need to identify. Buffer rotation grants neither data
immutability nor GPU completion. A frame count is not a fence; a nonblocking fence poll is needed
even for historical results, with not-ready meaning skip.

Keeping an immutable snapshot makes the delayed result valid **for that snapshot**. Processing it
later and writing over the current backbuffer can replace a newer image with an older one. A second
current-frame comparison or producer identity would be required to authorise that write. Presenting
the old snapshot intentionally is instead a buffered display pipeline with additional latency and
ownership, not a free admission trick for this host.

Delayed signatures remain useful diagnostics. Tag them with allocation generation, slot, source
revision if known, and the exact recording. Never promote them into authorisation for another visit.

### Reuse submission tracking

Keep the existing separation: `Completed` proves observed GPU completion; `Ready` additionally
requires sealing the list against replay (`DlssNr_SubmissionModel.h:48-51`). Use these properties
for scratch, source/cache and model lifetime, including the entire copy-out/NR/copy-back recording.
Unknown submission, failed Signal and removal never authorise reuse.

This solves *when retained data may be reused or freed*, not *whether its input was clean*.
There is also no blanket nonblocking guarantee: `DlssNr_Submission.cpp:196-206,230-249` uses mutexes,
and `Batch` serialises submissions. A no-wait admission interface must be designed explicitly;
calling `Ready` in a loop, moving the loop to a helper, or waiting on a worker's answer is forbidden.
No change to these production mechanisms is made by this note.

### Restrict the producer, or isolate its source

The recommended restriction is the contract below. A different possible architecture would keep
the game's clean render target separate from our displayed output by construction, for example a
cooperating final-image export or a fully virtualised backbuffer. That prevents our write-back from
becoming the next source, but requires more integration/lifetime work. It is not already implemented
by a scratch copy taken after contamination.

Restoring the original image after Present is not a safe shortcut: Present returning is not proof
that display consumption has finished, and restoration must also be ordered against the next game
write. Solving that requires a real buffer-reuse boundary; a new CPU fence wait is disallowed here.

## The restricted contract

An adapter is admitted only after inspection of a concrete producer path, not by executable name,
an INI assertion, a fullscreen draw, a clear, or a successful screenshot. For the first experiment,
that path is the harness's full upload from its independently generated CPU fixture.

Each offered image carries two records, described here as a protocol, **not a new production API**:

1. A **source record**: stream/generation, monotonically ordered clean revision, dimensions/format,
   and ownership of its immutable clean pixels. Repeated consecutive clean images retain the same
   revision; changed images receive a new one. The producer must know this from its image-production
   semantics, not number revisions using Present calls. Re-visiting a scene after other revisions
   can be a new revision; this is not an unbounded catalogue of every byte pattern ever displayed.
2. A **materialisation record**: the source revision, exact destination resource/generation, full
   image coverage, and actual submitted producer execution before this Present. It is single-use.
   No partial/scroll update, alias uncertainty, subsequent unaccounted write, or source dependency
   on any host-enhanced buffer is permitted. The producer serialises this boundary through host
   copy-out so the CPU record cannot race another writer. A command-list pointer or recording alone
   does not establish that the materialisation was submitted.

For a real target, the adapter audit must identify the clean source's producer, exclude dependencies
on previously displayed/enhanced buffers, identify all final writers, and bind the offered record to
their actual submission. A full CopyResource from an unknown source proves coverage, not cleanliness.
The revision mechanism must also be identified in that target's code. **Neither exists for a game in
this review.** If the adapter cannot establish both, that target stays unsupported. A read-only GPU
trace/readback test can challenge the audit; finite successful traces alone cannot prove the contract
for every producer path. Missing or changed paths must not silently reuse the adapter's assumptions.

Admission then has a checkable rule:

> Evaluate at most once for an admitted clean revision in a fixed NR configuration generation,
> and only from its full, ordered materialisation. A repeated materialisation never calls NGX again.
> An unknown input never calls NGX and never authorises write-back. Present admission never waits
> for GPU completion, a readback, another host thread, or an in-flight slot.

The selected generation also requires D3D12 SDR, no active/unknown FG, no second NR route touching
the same content, and the exact presenting DIRECT queue. First initialise outside the Present
decision, with separate setup submission and completed owned zero guides; do not run Create and
Evaluate together to make the first Present succeed. Keep only one outstanding NR composition using
the shared model state. Busy or unready means decline before recording work, not wait.

Maintain a consumed materialisation marker per destination and a revision ledger per selected
stream, not just per buffer. Reserve the revision before calling NGX so nested calls cannot both
evaluate it. Retain pending/attempted status separately from a completed usable answer. Old revisions
do not become new merely because the bounded output cache evicted them; retain a revision high-water
mark and decline old uncached revisions. Configuration/source/device changes invalidate the cache
and require a new clean materialisation; never reinterpret the backbuffer as clean after a reset.

### Outcomes, including failure

| Condition | Required action and safety argument |
|---|---|
| New valid revision/materialisation, host ready | Copy out, evaluate once, copy back only a successful complete composition; mark pending until submission/completion is proved |
| Same materialisation presented again | No NGX and no host write; it may already hold our answer, but is never used as fresh input |
| New full clean materialisation of the same revision | No new NGX. A completed cache entry with exactly matching source/configuration may be copied back; without one leave the clean game image alone. Never use a hash-only cache hit |
| New revision while previous host work is busy | No NR work or wait; leave the game's image alone. A later materialisation may offer the skipped revision again |
| Unknown/partial materialisation, changed allocation/queue, unexpected writer, absent source record | Refuse admission. No new model use or copy-back; this prevents further NR amplification, not a promise to repair pixels already modified by something else |
| Producer record exists but list was dropped or submission cannot be established | Reject it. The record does not prove an executed full rewrite |
| NR setup-only false return | Submit/seal only well-defined setup work with its own lifetime; no copy-back and no successful revision entry. Retry requires a valid materialisation and ready resources |
| Evaluate was attempted but failed, Close failed, or post-submit Signal/completion is unknown | Do not relabel as unattempted or reuse model/output. Disable admission for that generation and retain all possibly used objects. A failure after submission cannot promise to undo already queued writes; propagate the real error through the host's failure policy |
| Device removal | No reuse or cached answer; invalidate admission and use the separate dead-device lifetime path |
| Resize/teardown | Invalidate tickets before resource changes, retain pending objects, drain at the explicit lifecycle boundary or fail resize before destruction. Never initiate a blocking drain as incidental Present cleanup |
| Missing lifecycle/locking ownership proof | Unsupported, rather than waiting for a lock/worker in Present |

This is safe **under the verified producer contract**: fresh sources contain no host output; a
consumed source is never inferred to be new from changed bytes; pending work never authorises reuse.
A producer that lies about cleanliness or revision breaks that premise. The generic hook cannot
detect that lie from pixels; a checkbox claiming the contract is not a solution. The current harness
is the only eligible producer, so this proposal does not quietly declare arbitrary games safe.

Reusing a completed answer on a clean repeat avoids alternating enhanced/original images. A cache
miss or busy skip may still cause a visible quality change. That is a stated loss of enhancement,
not memory corruption or recursive processing; its frequency needs measurement before calling the
result playable. A change of NR tuning starts a new configuration generation after safe retirement,
and does not authorise reading the old enhanced backbuffer.

## No new CPU synchronisation also changes the flip proof

The old harness is deliberately synchronous: `present_nr.h:98,111,498,557` waits for its work, and
`:568-619` drains before resource release/resize. That run proves transport with those waits. It
does **not** prove this new no-wait contract. We must not retain its pre-flip wait and claim that
only the signature decision is asynchronous. A worker that is joined by Present is the same wait.

For a supported no-FG target, the proposed order is producer submission, host copy-out/NR/copy-back,
our overlay, then Present **on the actual queue supplied to that swapchain**. Restore PRESENT state
before handing off. D3D12 places presentation operations on that queue; GPU ordering, rather than a
CPU readback, is the applicable mechanism.
[D3D12 swapchains](https://learn.microsoft.com/en-us/windows/win32/direct3d12/swap-chains).

This is an API basis for a new validation, not a claim that our current Proton host has passed it.
Unknown interposers, another writer queue without established dependencies, or failure to prove this
order exclude the target. The donor's FG failure is not proof that every no-FG same-queue submission
needs a CPU wait. Conversely, the absence of FG alone does not certify queue identity.

The history is a warning, not a substitute for that proof: `555ed6db` removed the per-flip CPU wait
inside FG's synchronised present path while retaining the allocator-reuse wait; `afad5195` later
restored an optional pre-flip completion wait. Neither policy is imported here. The new hard
constraint also rules out adding an allocator-reuse wait on the Present thread.

The first restricted target uses one queue; it does not invent a wait on a future fence to stop the
present queue. A D3D12 queue Wait is GPU-side and returns immediately, but a cyclic dependency can
still wedge execution. It is not a blanket answer to the stall problem.
[Queue Wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait).

Nonblocking admission is also not already supplied by the mutex-based production Dispatch/tracker.
Their ownership/locking integration needs review before implementation can promise this contract.
Never wait for another thread's NR/resize/initialisation while holding the Present path. If this
cannot be achieved for an adapter, refuse that adapter. Ordinary CPU recording cost and blocking
internal to the application's original Present are not being promised away.

## Cost: bytes are known; milliseconds are not

At 3440x1440 RGBA8, `I = 19,814,400 bytes = 19.8144 MB` (decimal). The established transport is two
copies: **2I = 39.6288 MB payload**, or 4I logical read/write accesses. It stays full resolution when
model WorkingScale decreases. This is volume, not measured memory-bus traffic or a measured duration.

| Candidate operation | Additional work beyond an ordinary processed present |
|---|---|
| Full-content hash computed separately | Read I per image hashed, reduction/dispatch and barriers; generating the output witness separately reads another I. Fusion might reduce traffic but is not implemented or measured |
| Exact comparison against retained full output | Comparator reads two I-sized images, plus reduction/ordering; needs an I-sized witness per retained slot/generation. Capturing that witness may add a copy unless an immutable output is retained |
| Input plus output identity cache | Additional input/history storage and comparisons; a hash alone is insufficient for safe output substitution |
| Readback of the comparison bit/hash | Tiny result bytes do not remove the same-present dependency. Synchronous consumption is rejected; delayed consumption cannot authorise changed current content |
| GPU rejection with unconditional NGX | Comparator cost plus inference still paid; not a model-work optimisation |
| Producer admission metadata | Constant-size identity/ledger work, no image scan or admission readback. Obtaining truthful metadata is the integration prerequisite, not a free service from this tree |
| Reapply a completed answer to a clean repeat | One I payload copy to the backbuffer, no new model evaluation; retain an I-sized output cache and track its uses |
| Decline before recording | No additional GPU image work; the game presents its existing image |

Three retained full outputs alone use 59.44 MB logical image storage, excluding allocation alignment,
scratch, model surfaces and any inputs. No claim is made that this exact cache layout is necessary.
The producer's own normal full-frame render/upload is not counted as new host transport; creating a
new isolated producer path could add more work and must be costed separately.

The after-RR measurements of 2.90 ms at 0.5x and 6.36 ms at 1.0x in
[model-cost-vs-working-scale.md](model-cost-vs-working-scale.md) describe that measured route, not
latency or inference timing measured for this proposed host. They explain why skipping inference
matters; they do not price a GPU hash or predication. Model share also depends on scale. Admission
must not silently switch from RRWorkingScale=0.5 to WorkingScale=1.0; the eventual host must declare
its effective scale. That configuration choice does not solve provenance and is not made here.

Future timing must separate admission/comparison, copy-out, model, composition, copy-back and total
host work using the existing GPU timestamp/submission certificates. Collect completed samples later
without waiting in Present. Count attempted versus executed/copy-only/declined outcomes; timestamps
around a predicated region alone do not certify that NGX ran. Measure host CPU time separately, with
and without original Present, and report missing samples rather than synchronising to obtain them.

## How to falsify the contract in the harness

Extend the existing suite later; do not create a second model-only fake and call it host validation.
The harness already has full source control, a real swapchain, image readback and lifecycle fault
boundaries. Its producer knows the fixture revision without inspecting GPU results. Its independent
oracle must keep exact expected pixels and the actual executed writes, not trust the ticket under test.

Separate the producer, admission decision and observer. Keep the original synchronous probe as a
control. In the new experiment, readbacks are queued but consumed outside the measured Present path
after their fences complete. The test may wait while collecting evidence, never inside admission or
before each original Present to make the no-wait result pass. Retain per-attempt constants, sources
and readbacks until safe; the current single allocator/slot cannot simply have its Wait deleted.

| Sequence / fault | Required observation |
|---|---|
| Clean A, then the same materialisation offered twice | One actual NGX invocation; second entry has no NR recording/write-back |
| Full clean A uploaded again in the same or another slot | Same revision, new materialisation: no second inference; completed cache yields identical E(A), otherwise recorded safe skip |
| Clean B differs by one pixel, with a new valid revision | Process B once when host is ready; source readback must be exactly B, not a modified E(A) |
| E(A) retained, then only a HUD counter changes | Signature inequality would admit: record this as the negative control that disproves self-witness admission. Restricted contract has no clean materialisation and declines |
| An old E(A) copied to another slot | No clean-source record: decline despite a new destination index or full coverage |
| Dirty/scroll Present1 and overlay-after-NR changes | No inferred renewal from changed pixels; unknown provenance declines |
| Delayed comparison for A arrives after B replaced the slot | It cannot authorise B. Missing current evidence declines; immutable historical results stay historical |
| Forced signature collision | Rejection-only comparator loses an enhancement; no cached-image substitution and no claim that collisions test provenance |
| Ticket recorded but command list dropped/replayed, wrong generation/queue, resize with reused index | Reject invalid producer evidence; distinguish list recording from actual ordered execution |
| Busy host fence held by a test gate | Present admission returns a decline without waiting for gate release, with no allocator/resource reuse. Release the gate from the observer after this is proved |
| Submitted host write-back delayed before actual flip | Independent trace/capture proves write-back precedes the corresponding flip without a host CPU wait; backbuffer readback alone proves contents, not display order |
| Evaluate/Close/Signal failure, removal, resize while pending | No false completed cache entry or repeated attempt on uncertain history; no premature free; explicit error/retention behaviour |
| Fabricated producer ticket claiming mixed E(A) is clean | Independent pixel oracle must flag a producer-contract violation. Do not report a generic-host pass because the decision trusted fabricated metadata |
| NR disabled, unsupported producer, FG active/unknown | Zero new NR submissions and allocations on the default/unsupported route |

Use preserved buffers or explicitly populate the desired repeated bytes for repeat tests; do not
assume FLIP_DISCARD preserves the previous output. For displayed ordering, an instrumented presenter
or presentation trace must observe the producer/host/flip dependency. A pre-Present CopyTextureRegion
readback on the host queue, even if exact, cannot by itself prove what the display consumed.

Report model-call counts and recorded/submitted/completed outcomes separately, byte hashes plus
exact comparisons, MAE/max for images, and CPU wait/gate observations. Anchor parsed log records.
Repeat across two/three buffers, resize and index reuse. The comparison oracle may compute hashes
after the run; hashes must not secretly drive production admission synchronously.

Only after this protocol passes should a real adapter be considered. Its acceptance record must
name the executable/backend version, audited source production and revision sites, final write and
queue boundary, and unsupported alternate paths. Test pause, unchanged full redraws, menus, partial
updates, replay, resize and restart on that target. **No existing game is allowlisted by this note.**

## Review against DEVELOPMENT.md

- Default-identical: unsupported/off admission performs no capture, comparison, tracking-hook
  installation, allocation, model work or wait. No production toggle is added by this design.
- No dead controls / one quantity: no new menu or setting; source identity and materialisation are
  evidence, not user tuning knobs. A future enable key still requires the four-point round trip.
- No cbuffer or HLSL changes here. A later comparator experiment would need its own real compiled
  shader; do not modify NR's shared constant layout to disguise a new admission subsystem.
- Passthrough and zero-guidance semantics are unchanged. No claim is made that admission fixes HUD
  quality or model colour edits.
- Lifetime: preserve recording, submission, sealing and retirement guarantees. No new native Vulkan
  path. GPU predication never grants permission to free CPU-owned objects before actual completion.
- Local only. This document was checked against the cited code and primary API documentation; its
  adversarial cases and no-wait host have **not been implemented or run**. No build/tests were run
  for this documentation task. The per-change review is this explicit check; a future implementation
  still needs its own config, non-NR-path, lifetime and synchronisation review.

The next decision is whether to fund a verified producer adapter/source-isolation path. If the
requirement remains arbitrary games, present-only observation, opaque NGX and no new CPU wait,
the candidates examined here do not supply the missing clean-content proof.

---

## Review addendum (2026-09-14): the observer is not limited to Present

The indistinguishability argument above is correct, and it is correct **for the observer it assumes**.
"A present-only observer sees identical bytes" is the load-bearing phrase, and this tree is not a
present-only observer.

`resource_tracking/ResTrack_dx12.cpp` already hooks the command list, not the queue:
`hkOMSetRenderTargets` (`:1128`), `hkCreateRenderTargetView`, `hkDrawInstanced`,
`hkDrawIndexedInstanced`, `hkDispatch`, `hkExecuteCommandLists` and `hkClose`. The RTV-to-resource
resolution the question needs is there and working. A parallel review reached the same negative
conclusion by reasoning about "signals at the D3D12 *queue* level", which is true and does not
cover this: the signal is one level below the queue.

**What that buys, stated narrowly.** It settles the case the feedback loop actually needs, which is
the *negative* one: if nothing bound `B_i` as a render target and drew into it, and nothing copied
into it, since we wrote `E(C)` there, then `B_i` still holds our output and must be refused. That is
observation rather than producer cooperation, and it does not depend on pixels.

**What it does not buy.** It does not establish full coverage, and it does not establish that the
pixels a draw wrote were independent of our previous output. The indistinguishability example
survives both. So this converts an unbounded question into a bounded one — from "prove these pixels
are clean" to "prove this write covered the frame and did not read our output" — and bounded is not
solved.

**And it is not free.** Those hooks are gated: `:426` requires `FGEnabled` *and* `FGHUDFix`, and
`hkOMSetRenderTargets` early-exits unless `IsHudFixActive()`. They are armed today only for HudFix,
which needs frame generation — which this slice refuses. Arming them for a present host means a
second arming condition and per-draw-call overhead on every frame, which is precisely why they are
gated in the first place. Nothing here measured that overhead.

So the verdict above stands as written for the first slice: the harness is the only eligible producer
and no game or emulator is verified. What changes is the shape of the open question. It is not
"provenance is unobtainable"; it is **"is command-list observation of the backbuffer's writers cheap
enough, and does it establish coverage?"** — and that is a question with an experiment attached
rather than a proof against it.

### One correction to the parallel review

That review inferred that an emulator blitting every vblank would cause *accumulation of
enhancement*. It would not. If the blit writes a clean `C` over `E(C)` on every present, the input is
clean every time and the output is `E(C)` every time; nothing stacks. What it causes is **duplicated
cost** — two evaluations per emulated frame at 30 fps on a 60 Hz display — and whatever the model's
internal temporal history does with two identical inputs at advancing time, which is a separate and
unmeasured question. Conflating duplicated cost with recursive enhancement makes the emulator case
look like a correctness problem when the evidence says it is a cost problem.

This matters because the two requirements have different answers. **Feedback safety needs provenance;
duplicate suppression does not.** A signature of our own output is unusable for the first, for the
reasons proven above, and perfectly usable for the second: as a rejection-only rule its worst failure
is losing one enhancement, which the note already concedes. The candidate was not wrong, it was
assigned to the wrong requirement.
