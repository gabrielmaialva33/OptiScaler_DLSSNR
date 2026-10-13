# NR present-guide identity (host)

```bash
python3 tests/nr-present-guide-identity/run.py
python3 tests/run_all.py --only nr-present-guide-identity -v
```

Step 0 of `docs/dlssnr/design/present-real-guides.md`: the identity layer for the present guide ring, built log only.
The runner does three things, under g++ C++20, ASan/UBSan and `-Werror`.

1. **Source guards** on the renderer (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp`), the bridge host and the feature
   header, checked first.
2. **`decide.cpp`** compiles the production header `retoque/dlssnr/DlssNr_PresentGuideIdentity.h` with the production
   ring `retoque/dlssnr/DlssNr_PresentGuideRing.h`.
3. **The glue as the renderer has it**: the block from `struct PresentGuideIdentityState` up to
   `ReleasePresentTemporal` is sliced out of the renderer, compiled after `glue_fakes.h` and run by `glue_cases.h`. A
   glue that grows a dependency stops compiling here.

## What this covers

- **Every rule failing alone**, with the reason it reports: device (and unknown devices), extent (and an unknown
  output), output base, swapchain, ambiguous (an ordinal above one, a newer capture of the same frame and extent still
  in the ring, an unknown ordinal), consumed, lag (one boundary behind passes, two do not, a stamp ahead is not
  behind), stale (and a zero clock), subrect (zero, wider, taller, equal to the output).
- **First-failing order**: a capture that fails all nine checks reports `device`, and mending one at a time walks the
  table of D2 in order to `accepted`.
- **Narrowing only**: 50000 random rings, stamps and presents. The answer is always PickForPresent's slot or none,
  accepted exactly when no rule fails, otherwise the first failing rule by a table-driven oracle; every reason occurs.
- **The swapchain stamp's null semantics**: no stamp matches a present that sees exactly one presenting swapchain on
  its device and nothing else (two, or none); a stamp must be the presenting chain, however many there are.
- **The registry and the frame counter**: presenters per device, expiry after `kPresenterLiveMs`, the cap; ordinals per
  device and output extent restarting at every boundary, unknown past `kExtentsPerFrame`; `RenderExtent`'s fallback.
- **Scenarios on the production ring and tracker**: one capture per present (all accepted, lag 1); the first capture
  before any present; frame generation at 2x and 4x (the generated presents are consumed); a copy that completes late
  (consumed, then lag 2); portrait then main and main then portrait (accepted, extent); two evaluates at one extent
  (ambiguous whichever is picked); a resize; another device; a second swapchain presenting, and it going quiet; a
  recreated swapchain. A 20000-frame random run keeps the record's books balanced.
- **Step 0's record**: what a window counts (today's read, skip and none; the decision over reads; skips the rules
  would accept; each capture's lag and frame count once, where it is first offered), the exact line for two windows,
  and the rate: nothing inside 2 s, a window like the last one written held to 30 s and carrying its whole interval, a
  change written at 2 s, about 120 lines an hour in a steady session.
- **The glue**: inert on an empty ring (no boundary, no count, no device reference), the stamp's reads (the output's
  extent and base under the evaluate's names, the subrect or the depth extent), today's read, skip and none as
  `RunPresentPass` decides them, the ring byte-for-byte unchanged by a note, the bridge not counted while the pass is
  off or with no presenter, a device reference given back.
- **No behaviour change**, by source guard: `RunPresentPass` and `CapturedPresentGuides` call their note right after
  their one `PickForPresent`, still read `pickedSlot`, and never name a decision; the observers return `void`; the glue
  never picks, records, invalidates or tracks a slot or touches a clone's read bookkeeping; the note and the pass skip on
  one test; the stamp is written after both copies and right before `CommitRecord`; the one log call is gated by the
  window; shutdown resets the identity with the ring; and at the default `HookMethod=1`, or with the pass off,
  `RunPresentPass` returns before the pick.

## What this does not cover

No D3D12, no GPU, no game. Whether the stamps a real title produces agree with its presents is exactly what step 0's
log line is for, and only a run says it. The device identities are compared as pointers the way production compares
them; whether two interfaces of one device ever reach the two sides as different pointers is not something a fake
can show. The real `Track` that follows the pick (and can refuse) is not modelled: the note counts a pick as read
before `Track` runs. The D3D11 bridge's use of the ring has no test title (open question 4 of the note).
