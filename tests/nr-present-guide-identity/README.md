# NR present-guide identity (host)

```bash
python3 tests/nr-present-guide-identity/run.py
python3 tests/run_all.py --only nr-present-guide-identity -v
```

Step 1 of `docs/dlssnr/design/present-real-guides.md`: the identity layer for the present guide ring, applied. A
present reads a capture only when every rule accepts it, and falls back to zero or synthesized guides otherwise. The
runner does three things, under g++ C++20, ASan/UBSan and `-Werror`.

1. **Source guards** on the renderer (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp`), the bridge host, the menu and the
   feature header, checked first.
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
- **Rule 6 enforced and not**: where the present does not enforce it (every entry but the game's own Present), a
  capture past the limit is accepted and marked, and the rules after it still refuse; a refusal never carries the mark.
- **First-failing order**: a capture that fails all nine checks reports `device`, and mending one at a time walks the
  table of D2 in order to `accepted`.
- **Narrowing only**: 50000 random rings, stamps and presents, with the lag limit enforced on two in three. The answer
  is always PickForPresent's slot or none, accepted exactly when no rule fails, otherwise the first failing rule by a
  table-driven oracle; the mark exactly when it should be; every reason occurs.
- **The swapchain stamp's null semantics**: no stamp matches a present that sees exactly one presenting swapchain on
  its device and nothing else (two, or none); a stamp must be the presenting chain, however many there are.
- **The registry and the frame counter**: presenters per device, expiry after `kPresenterLiveMs`, the cap; ordinals per
  device and output extent restarting at every boundary, unknown past `kExtentsPerFrame`; `RenderExtent`'s fallback.
- **Scenarios on the production ring and tracker**, with rule 5's record moved only by a use: one capture per present
  (all accepted, lag 1); the first capture before any present; frame generation at 2x and 4x (the generated presents
  are consumed); a copy that completes late (consumed, then lag 2: refused where enforced, used and marked where not);
  the wrapped entry as measured, every capture two behind (none used where enforced, each used once where not);
  portrait then main and main then portrait (accepted, extent); two evaluates at one extent (ambiguous whichever is
  picked); a resize; another device; a second swapchain presenting, and it going quiet; a recreated swapchain.
- **A long run read the way step 1 reads**: 20000 frames on the production ring with drops, late copies, captures at
  another extent or device, generated presents and entries alternating between enforcing the lag limit and not. The
  present tracks the decision's slot and nothing else, and a read stays in flight for a few presents. Held: the
  decision's slot never names a capture refused earlier for a reason that cannot improve (device, extent, output base,
  render extent, an earlier use, age); a slot being read is never overwritten; the record's books balance. (That a
  refused pick is not read at its own present is the source guards' to hold: this run reads what the decision gives.)
- **Which refusals settle the newest capture** (`SettlesNewest`): those that live in the capture and none that turn on
  the present; over 20000 rounds on the production ring, a newest capture refused for one of them is refused again by
  every later present with nothing newer (so the skip after it passes over nothing the rules would accept), while one
  refused on extent is accepted by the next present at the settled size.
- **Step 1's record**: what a window counts (used, refused by reason, the pass's own skip and the skips the rules would
  accept, none, the presents that enforced the lag limit, the uses past it where it was not enforced, each capture's
  lag and frame count once), that only a use moves rule 5's record (an untracked read is none), `MarkUsed` never going
  backwards, the exact line for two windows, and the rate: nothing inside 2 s, a window like the last one written held
  to 30 s and carrying its whole interval, a change written at 2 s, about 120 lines an hour.
- **The source-change reset** (`SourceHistory`): a single miss or an alternation never resets; a change held for
  `kSourceStable` presents resets once, in either direction; the first source to hold is the start; a random run never
  resets twice inside `kSourceStable` presents and only on a held change.
- **The status**: packed into one word and back (saturating), one second at a time with skips apart and the most
  frequent refusal named, which source a second names from the host's own report (zeros, synthesized, waited), the
  status line's literal for each (never the latch's old "DLSS temporal inputs"), and the menu's guides line in each
  row of DEVELOPMENT.md section 2.4.
- **The glue**: the swapchain's window as the presenter (the swapchain when it has none), so the Streamline proxy and
  the swapchain beneath it are one presenter; inert on an empty ring over many presents (no boundary, no count, the
  bridge's list not asked for its device, no log, no reset), with the source history settling on zeros; the stamp's
  reads; the D3D12 present's use, consumed, skip, lag at and below the game's Present, extent, a second window; the
  change from zeros to the game's guides (one reset, its line), three refused frames that still hold the history (no
  reset, and the hold `ZeroGuideReset` waits on), the fourth that resets, the way back, and a route that starts again;
  the status published once a second, its source from what the host reported (waited, zeros, synthesized) and the menu
  line, gone two seconds after the last present; the log line under its new prefix; the bridge with the pass off, at
  another HookMethod, with no presenter or no device, and on, and an unlogged bridge whose use still makes the same
  capture consumed; asking never changes the ring.
- **Source guards**: `RunPresentPass` and `CapturedPresentGuides` ask right after their one pick and read only the
  decision's slot (the pick goes to the identity and the record and nowhere else); the bridge's read tracks before it
  hands anything out; the skip stays in front of the read and is counted; the source is noted only for a pass that
  runs; the fallback is noted after the synthesized motion; the glue never picks, records, invalidates or tracks a slot
  or touches a clone's read bookkeeping, and its only write outside its own state is the reset; two gated log calls;
  rule 6 enforced only at the boundary and never on the bridge; `HookStatus` reads no latch; the stamp is written after
  both copies and right before `CommitRecord`; shutdown resets the identity with the ring; and at the default
  `HookMethod=1`, or with the pass off, `RunPresentPass` returns before the pick.

## What this does not cover

No D3D12, no GPU, no game. Whether the stamps a real title produces agree with its presents is what the log line is
for, and only a run says it. The device identities are compared as pointers the way production compares them;
whether two interfaces of one device ever reach the two sides as different pointers is not something a fake can show.
That a real driver accepts the copy formats, that the pairing is visually right, and that DLSS-G does not stall are not
shown here either. The D3D11 bridge's use of the ring has no test title (open question 4 of the note).
