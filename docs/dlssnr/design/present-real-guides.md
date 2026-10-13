# Real guides for the present pass: proving the capture belongs to the frame being presented

Status: design for review, 2026-10-13. **Step 0 (log only) is built**, on branch `feat/present-guide-identity`:
the stamp, the decision header and both consumers' calls, acting on nothing; see
[Step 0 as built](#step-0-as-built). Steps after it are not built. Nothing here has been run in a game. Written
against `93730d39`; line numbers are into that tree.

Read first: [nr-present-hook.md](nr-present-hook.md) (the present host, and the ring this note builds
on), [synthesized-motion.md](synthesized-motion.md) (what runs when there are no game guides),
[nr-dx11-bridge-host.md](nr-dx11-bridge-host.md) (the second consumer of the ring),
[reduced-scale-guides.md](reduced-scale-guides.md) (the conversions the pass already does), and
[DEVELOPMENT.md](DEVELOPMENT.md) (the invariants the design has to anticipate).

## The question, and a correction to its premise

The question that started this note: in a game that calls an upscaler somewhere in the frame, so
OptiScaler sees its depth and motion vectors, but where the user wants the pass at present, could the
game's real depth and motion be matched to the presented frame instead of estimated?

**Most of that already exists.** The present pass does not run only on zero guides or synthesized
motion. `CaptureTemporal` (`retoque/shaders/dlssnr/DlssNr_Dx12.cpp:5775`) copies the game's depth and
motion into an owned four-slot ring on every upscaler evaluate while `HookMethod=2`, or `HookMethod=0`
with the present hook live (`:7086-7088`). `RunPresentPass` picks the newest eligible slot
(`:6477`) and runs the pass with the game's own `DlssNrFrameInfo` (`:6511`). Zero guides
(`DlssNr_ZeroGuides`, `:6543`) and then synthesized motion (`:6630`) are the fallback for a frame with
no pick. The D3D11 bridge's present host takes the same ring through `CapturedPresentGuides`
(`:7493`, called at `retoque/dlssnr/DlssNr_PresentHost.cpp:423`), which is what happens when a D3D11
title runs an upscaler through the Dx11wDx12 interop. The comment there says so in as many words.
The `RequireDlss` text in `OptiScaler.ini` once said "the D3D11 bridge's host always runs on constant depth and
no motion", which predated that; it was corrected on 2026-10-13 (`ec8a5e61`).

What was measured about this route is small. The ring's selection logic has a host suite
(`tests/nr-present-guides`). The route ran in Crimson Desert on 2026-10-04 with DLSS-G off (68 presents
to 68 renders, NR on each; `nr-present-hook.md`, 2026-10-04 update). Nothing records an image
comparison of real-guide present against after-upscale, and the depth-copy formats on native Windows
are listed there as unproven.

**What does not exist is a proof that a capture belongs to the frame being presented.** The ring
decides by age (250 ms), by whether the evaluate's submission has completed or ran on the present
queue, and by whether a newer capture exists than the last one used. That is a freshness test, not an
identity test. The rest of this note designs the identity layer, the fallback when it fails, and the
parts of lifetime and synchronisation that the identity layer changes.

## What the ring knows, and what it does not

Per slot, at capture (`PresentGuideClone`, `:5634`; `Slot`, `DlssNr_PresentGuideRing.h:21`):

| Known | Where |
|---|---|
| A sequence number, the capture time, the depth extent, whether depth is inverted | `CommitRecord`, `:5962` |
| The device the clone was made on | `clone.deviceIdentity`, `:5872`; **read at capture only, never compared at present** |
| The game's frame description: render subrect, subrect bases, motion scale, `MVLowRes`, reset | `clone.frame`, `:5954`, from `GatherFrame` (`:6807`) |
| The submission that wrote the copy, and the one that read it | `captureWriteUsage`, `presentReadUsage` |

What a present can say about itself at `RunPresentPass`: the swapchain, the backbuffer's index
(`:6384`), its device, its extent, the queue it was called with, whether this is the FG hook.

What nothing records, and which therefore cannot be checked:

1. **Which swapchain the guides are for.** An evaluate has no swapchain. `nr-present-hook.md` already
   names the consequence for globals (the ring is one file static, hazard 2 there); for guides it means
   a second swapchain presenting (a launcher, a tool window) can be paired with the game's guides.
2. **The extent of the image the upscaler produced.** `clone.width/height` is the depth extent, at
   render size (`:5957`). A game that scales, letterboxes or crops the upscaler's output before present
   presents guides that describe a different picture, with nothing in the ring to say so. The pass
   reads guide and colour extents separately, so it will run, and it will pair edges that do not line
   up.
3. **How many evaluates fed this present.** Two evaluates in one frame (an offscreen portrait or
   split screen, the case `ExtentIsStable`'s comment in `retoque/shaders/dlssnr/DlssNr_Common.h:156` describes) both
   capture; the pick takes the newest. If the newest is the portrait, the main view is presented with
   the portrait's depth.
4. **Whether the present is the frame the capture was made for.** The newest-sequence test
   (`:6485`) asks "is there something unused"; it does not ask "is it this frame's". The only frame
   count the module keeps (`g_presentFlip`, `g_renderSeq`) is never compared across the two sides.
5. **Whether the upscaler is still running.** `g_temporalValid` is written in two places only,
   `true` at `:5964` and `false` at `:5772` (shutdown). It is a latch, not a freshness signal. It
   decides the Auto route (`:6282`, `:6333`, `:7090`) and the status line, which keeps saying
   "swapchain source with DLSS temporal inputs" (`:6283`) after the upscaler has gone quiet and the
   frames are running on the zero-guide branch. Routing on the latch is harmless today, and arguably
   wanted: once the evaluates stop, the after-upscale route has nothing to act on, and the present
   route carrying on with zero guides is what `nr-menu-mode.md` asks for. The status being wrong is the
   defect.
6. **The output subrect.** `GatherFrame` reads the render subrect and the depth and motion bases, not
   `DLSS.Output.Subrect.Base`. A non-zero output base means the upscaler wrote into part of a larger
   texture, and a present of the whole texture is not that picture.

## Prior art: NeuRotic

NeuRotic's "Present Enhanced" (the clone reviewed on 2026-10-13, tip `7132a103`, GPL-3.0 like this
tree; nothing was built or run, and nothing is copied) arrived in `f8583cca` and `90895cf8` (2026-09-10) as a port of a captured-guide
route, then grew through `f63e9e37` and `623881ab`. Its matching layer is the bulk of what it
adds, and it is worth reading as a catalogue of what can go wrong:

- **Swapchain identity by private data.** `PresentGuides::Identity()` (`dlssnr/DlssNr_PresentGuides.h`)
  stores a COM cookie on the swapchain with `SetPrivateDataInterface` under a fixed GUID, so the
  wrapper and the real object report the same identity. Registered swapchains are tracked per device,
  and the capture is refused when more than one swapchain shares the device (`UniqueChain`,
  `dlssnr/DlssNr_Dx11.cpp`).
- **A contract record.** `CapturedGuideContract` (`dlssnr/CapturedGuideContract.h`) carries device,
  device generation, swapchain, swapchain generation, acquisition, image, stream, capture, previous,
  and extent, and `Matches()` requires every one. Its own comment: "Capture-owner identity, never an
  authenticated engine/provider frame token". That is the honest limit, and it applies here too:
  nothing in an NGX parameter block says which frame it belongs to.
- **One capture per present.** A capture count per present interval; two or more refuses the frame
  ("Multiple Native evaluations before Present; no guide pair used", `Bridge::Capture`).
- **The render subrect must be real.** Zero, or larger than the output, refuses
  (`frame.RenderSubrectWidth > width`, same function).
- **Generations.** Any swapchain creation, resize or destruction, and any feature change, bumps a
  generation and drops every pending capture (`Bridge::Invalidate`).
- **An input policy** of image-only, require-guides, or auto-guides
  (`dlssnr/DlssNr_PresentInputPolicy.h`), which is the same three states `RequireDlss` and
  `SynthMotion` already span here between them.
- **Producer proof, not a counter.** For their D3D11 route a shared fence and the queue that waited on
  it are the evidence the producer finished, in addition to a command-list ticket. This tree's
  equivalent is `Submission::Completed` / `SubmittedOnlyOn`.

What not to take: their code is a dense one-line-function style in 30-odd contract headers, its
synchronisation assumes Windows-native queue ownership (their own Streamline handling is a failure
catalogue for Dragon's Dogma 2 dialogue dropouts), and it has never merged upstream OptiScaler, so
none of it would apply to this tree without rewriting. The ideas are the prior art: identity as a
record with a generation, refusal over guessing, one capture per present.

## The design

### D1. One ring, one new decision header, no second capture path

Do not add a second capture. Extend what exists:

1. **A stamp on each capture**, written by `CaptureTemporal` alongside the sequence:
   - the device identity (already stored);
   - the evaluate's output extent and output subrect base (from the `Output` resource and
     `DLSS.Output.Subrect.Base.X/Y`; both new reads in `GatherFrame`);
   - `presentEpoch`: the value of `g_presentFlip` when the capture was made;
   - `evalsSincePresent`: a per-device counter of captures since the last present consumed one;
   - the swapchain, **only when it is unambiguous**: `State::currentSwapchain` if exactly one swapchain
     is registered on that device, else null. A null stamp is "unknown", and unknown is not a match
     for a present that can see two swapchains on the device.
2. **A pure decision header**, `retoque/dlssnr/DlssNr_PresentGuideIdentity.h`, standard library only,
   the pattern of `DlssNr_PresentSync.h` and `DlssNr_PresentGuideRing.h`: it takes the present's facts
   and the stamps and answers `{slot, reason}`. `RunPresentPass` and `CapturedPresentGuides` both call
   it, so the two consumers cannot drift (the way the two present transports drifted on
   `ExtentIsStable`, per the comment at `:6650`).
3. **The ring stays the owner of slots, completion and retirement.** The decision header runs after
   `PickForPresent`'s eligibility test and may only narrow its answer, never widen it. An identity
   failure cannot make an unsafe slot readable.

### D2. The rules

A slot is accepted for this present only if all hold; the first that fails is the reason logged.

| # | Rule | Source of the facts | On failure |
|---|---|---|---|
| 1 | Same device as the backbuffer | stamp vs `backbuffer->GetDevice` | refuse; `device` |
| 2 | Capture's output extent equals the backbuffer's extent, and its output base is (0,0) | stamp vs `bbDesc` | refuse; `extent` or `output-base` |
| 3 | Swapchain stamp is the presenting swapchain, or null with exactly one swapchain on the device | stamp vs `swapchain` and the registry | refuse; `swapchain` |
| 4 | Exactly one capture with a matching extent since the last consumed present; or several, all but one older than a frame boundary that a present marked | `evalsSincePresent`, `presentEpoch` | refuse; `ambiguous` |
| 5 | The slot's sequence has not been consumed by an earlier present | ring sequence vs last consumed | skip this present's pass; `consumed` (the generated-frame case) |
| 6 | Epoch lag within bound: `presentEpoch >= g_presentFlip - kLag` | stamp vs `g_presentFlip` | refuse; `lag` |
| 7 | Age within `kPresentGuideMaxAgeMs` | existing | refuse; `stale` |
| 8 | The render subrect is non-zero and no larger than the output | `clone.frame` | refuse; `subrect` |

`kLag` is the one number this note cannot choose. Engines with a separate render thread submit the
evaluate for frame N while the present thread is still presenting N-1, so the epoch of a correct
capture is not always equal to the present's. It is assumed to be 1; it must be measured first (see
the test plan, step 0) and the design stands or falls on that measurement. If no small constant
describes a title, rule 6 is dropped for it and rules 2 to 5 carry the identity.

Rule 4 deserves its own sentence, because it is where this design differs most from today. Today the
newest capture wins. Under rule 4 a frame with two same-extent evaluates is refused rather than
guessed. That is the fail-closed choice NeuRotic makes; the cost is that a game which evaluates twice
per frame at one extent never gets real guides at present, and falls back. That is a title to read
the log for, not a title to guess at.

### D3. What a refusal does: fall back, and never use what failed

On any refusal the frame takes the branches that exist now: zero guides, with synthesized motion
when `SynthMotion` is on. The stale or mismatched slot is never read: the decision runs before the
read is tracked (`:6496-6523`), so there is nothing to undo on the slot.

Two things are new and both are about the model's history, which is the part of this that is easy to
get wrong:

- **A source change is a history event.** Real motion and zero or synthesized motion are different
  contracts for the same history buffer. The module already resets on a route switch
  (`g_sourceIsPresent`, `:6604`). The same should hold across a guide-source change, but a title
  that alternates every few frames would then reset continuously, which is the failure
  `ZeroGuideReset` documents. So: a single-frame miss falls back without a reset; a change that holds
  for `kSourceStable` consecutive frames resets once. `kSourceStable` is assumed (4 is the starting
  guess) and is to be tuned from step 0's log.
- **The status stops reading the latch.** The status line and the menu line take their guide source
  from the decision's own recent answer ("accepted in the last N presents"), so they say "zero" or
  "synthesized" when that is what is running. Routing does not change: Auto stays on the present
  route after the upscaler stops, so the zero-guide frames in a menu or cutscene keep being edited,
  which is the behaviour `nr-menu-mode.md` is trying to preserve.

### D4. Lifetime and synchronisation

The design adds no resources and no queues. The reasons it needs saying anyway:

- **Copies stay on the evaluate's list, on the evaluate's queue.** `CaptureTemporal` tracks the list
  before any barrier (`Submission::Track`, `:5906`) and records the copy and the barriers around it
  there. The present reads the copy on the present list, tracked as `presentReadUsage`.
- **No GPU cross-queue wait.** The pick admits a slot only when its write was submitted only on the
  present queue (queue order proves completion) or has completed on the CPU. A capture made on an
  async compute queue that has not completed is simply not eligible and the frame falls back. The
  alternative, a GPU wait on the producer's fence, is what the 2026-10-04 update of
  `nr-present-hook.md` avoids because of DLSS-G deadlocks, and the Dx11wDx12 bridge avoids a GPU-side
  wait for a related reason under vkd3d-proton (several D3D12 queues can map onto one `VkQueue`,
  `with_dx12/dx11_with_dx12.cpp:243`); this note does not reopen it. The cost is lost frames on titles whose evaluate is on a different queue; the
  log reason makes that visible.
- **Never freed under the GPU.** Clones are parked (`ParkNrClone`, `:5740`) and retired by evidence
  through `TickNrRetired`; a rebuilt clone (size or format change) goes through the same path. The
  identity change does not touch this. The one new lifetime event is a **resize**: nothing flushes the
  ring on a swapchain resize today (`ReleasePresentTemporal` is called from shutdown only,
  `:8590`). Rule 2 makes the old captures unusable at the new extent; the slots are overwritten in
  due course. A generation bump on resize (as NeuRotic's `Invalidate`) is worth adding so the stamps
  of a previous swapchain can never match, and it is cheap: it is a counter in the stamp, not a free.
- **Cost when unused is zero**, and that is already true: `CaptureTemporal` is reached only at
  `HookMethod=2`, or `0` with the present hook live (`:7086`). At the default `HookMethod=1` nothing
  here runs: no stamp, no registry, no decision. A title with no evaluate (PCSX2) never captures, so
  the decision never runs and the ring is never allocated. VRAM when used is as `nr-present-hook.md`
  states: four slots of depth and motion at render size, about 118 MB at 2560x1440 with 4-byte formats.
- **Locking.** The decision reads the ring and the stamps under `g_nrMutex`, exactly where
  `PickForPresent` is called today (`:6477`, with the lock held until `:6541`). It adds no new lock and
  no new wait on the present thread.

### D5. Frame generation

The pass runs once per base frame, before FG, in both FG presenters (`:6312-6326` stands the wrapped
entry down once the FG hook ran). With identity in place that has a precise meaning:

- A generated frame has no capture of its own; rule 5 (consumed) refuses it. Today's
  `++g_presentSkips` branch at `:6485` is the same decision made implicitly. It is the correct one:
  applying the base frame's guides to a generated frame would pair motion with the wrong instant and
  advance the model's history on duplicates (the 4x arithmetic in `nr-present-hook.md`).
- The visible consequence is the one already measured: with the game's DLSS-G on, `HookMethod=2` edits
  the real frames and the generated ones reach the screen unedited, which reads as flicker (Crimson
  Desert, 2026-10-04). Real-guide identity does not change that and this note does not try to. The
  advice stays `HookMethod=1` under DLSS-G. Running at DLSS-G's real-frame boundary, which NeuRotic
  has (`623881ab`), is a separate task with its own queue-ownership risk.
- The DLSS-G interlock (`menuOverlayIsVulkan`, the Vulkan present guards) is Vulkan-side and is not
  touched; the D3D12 present sync (`DlssNr_PresentSync.h`) is unchanged and still keys on whether
  frame generation could be involved.
- Synthesized FG (`FGInput::Synthesized`) has no upscaler evaluate, so no capture exists and the
  identity layer is inert there by construction.

### D6. Conversions: unchanged, and why that is not an oversight

The present pass calls the same `Dispatch` as the after-upscale route with the capture's own
`DlssNrFrameInfo`, so the conversions in `reduced-scale-guides.md` (motion scale in the model's
motion texture's pixels, guide resample below working scale 1, `MVLowRes`, subrect bases) apply to
present exactly as they do after the upscaler. What is the same, and therefore costs no new risk:

- **Motion scale and space.** The scale and the `MVLowRes` flag are read by `GatherFrame` from the
  game's parameter block and travel in the capture. Render-resolution vectors against output-sized
  colour is the same relation the after-upscale route has.
- **Depth inversion.** `frame.DepthInverted` comes from the create flags, rides in `clone.frame`, and
  is stored in the ring slot. Zero guides are not inverted (`PresentFrameDefaults`, `:7593`).
- **Jitter.** This tree passes the model no jitter at all (no `Jitter_Offset` read or written in
  `DlssNr_Dx12.cpp`), and `GatherFrame` does not read `MVJittered`. The colour at present is the
  de-jittered output, and the guides are jittered renders, which is also true after the upscaler,
  where it has been measured working (Crimson Desert). So present adds no jitter risk beyond that
  route's. NeuRotic's D3D11 route refuses `MVJittered` outright ("jittered motion-vector convention
  is not validated", `DlssNr_Dx11.cpp`); whether to mirror that refusal here is an open question below.

What is different at present, and is covered by the identity rules rather than by conversion: the
image is the backbuffer, not the upscaler's output. If they differ in extent (rule 2) or offset
(rule 2, output base), the guides do not describe it. If they match in extent and offset but the game
drew its HUD, film grain or a post-upscale distortion after the upscaler, the guides describe the scene
and not those additions. That is the property of running at present that `nr-present-hook.md` states
("without a HUD-less backbuffer the model will denoise text"); `UiProtection` is the existing
mitigation. It is also the stated reason to run at present at all, so it is a trade the user makes,
not a defect this note can remove.

## Config surface

**No new key in v1.** `HookMethod`, `RequireDlss` and `SynthMotion` already span the states a user
can ask for (guides required; real else zero; real else synthesized). A fourth control over the same
quantity would break "one quantity, one control" (`DEVELOPMENT.md` §1.3). What changes:

- Behaviour: guides are used when identity is proven, and refused with a reason otherwise. At the
  default `HookMethod=1` this is inert. Under `HookMethod=0` or `2` it is a correctness change to an
  opt-in route (captures that used to be used may now be refused), not a new toggle, so invariant 1
  ("default-identical") holds at defaults and the review below treats the route change as the
  subject.
- Menu: one read-only line under the present controls, "Guides: game's, N refused (reason)" /
  "synthesized" / "zero", shown only when the present route is active (invariant 2, no dead control).
- Log: one line per window, in the style of the existing two-second status line, with counts per
  refusal reason, so a title can be diagnosed from its log alone.
- **Reserved, not built:** a strictness key (`PresentGuideIdentity=strict|relaxed`) only if step 0's
  data shows a title that rules 4 or 6 refuse while its guides are demonstrably right. It would be a
  four-point config round trip and is not added speculatively.

## Review against DEVELOPMENT.md

| Item | Anticipated answer |
|---|---|
| 1 Default-identical | At `HookMethod=1`: no stamp, no registry, no decision, no log. Route change confined to 0 and 2. |
| 2 No dead control | The menu line only appears with the present route active. |
| 3 One quantity, one control | No new key. |
| 4 Config round-trip | Not triggered (no key). Triggered if the reserved key is ever added. |
| 5 Struct equals cbuffer | No shader or constant change. |
| 6 Shader rebuild | No shader change. |
| 7 Passthrough | The decision is upstream of the encode and does not touch it. |
| 8 Vulkan lifetime | D3D12 only. Vulkan present is `nr-vulkan-present-host.md`'s subject. |
| Non-NR-path touch | Inert with NR off: `RunPresentPass` returns first (`:6294`). |

## Test plan

**Step 0, before any behaviour change: measure.** A log-only build of the stamp and the decision
(it records what it would have refused and why, and acts on nothing) in Crimson Desert with DLSS-G
off, under `HookMethod=0` and `2`, for a session that includes a menu, a cutscene, a resolution change
and Alt-Tab. The numbers to take: the distribution of `g_presentFlip - presentEpoch` (decides
`kLag`), how often two evaluates share a present (decides rule 4), how often the output extent differs
from the backbuffer's (decides rule 2), and whether any stamp disagrees on device. If the data says
the identity rules would refuse most of a session that currently looks right, the design is wrong and
this note should be rewritten before step 1.

**Host suite, new: `tests/nr-present-guide-identity`** (host tier, registered in `tests/suites.toml`),
built on the production decision header with fakes, on the same model `tests/nr-present-guides` uses
for the ring and `Submission::Detail::Model` for completion. Cases: every rule failing alone and the
reason it reports; a second swapchain on the device; portrait-then-main and main-then-portrait
evaluates; a resize between capture and present; a generated frame (rule 5) at 2x and 4x; the
source-change debounce (a single miss does not reset, `kSourceStable` misses reset once); a long
simulated run with random capture drops, asserting that **no slot is ever read after the rule that
refused it** and that the ring's never-overwrite-an-in-flight-slot property still holds. Plus an
`nr-invariants` addition: no reader of `g_temporalValid` outside the status line.

**What the host tier will not show**, and the README of the suite should say so: that a real driver
accepts the copy formats, that the pairing is visually right, that DLSS-G does not stall.

**In-game, with our titles:**

| Title | What it checks |
|---|---|
| Crimson Desert (D3D12, DLSS-G off) | `HookMethod=2` and `0`: refusal counts by reason across a session; image against `HookMethod=1` under fast camera motion, same frame via Hold frame where possible; no ghosting that `ZeroGuideReset` was written to cure. |
| Crimson Desert (DLSS-G on) | Expect rule 5 refusals on generated frames, no stall, no `887A0001`, PresentSync unchanged. Flicker is expected and is not a failure of this change. |
| Divinity (D3D11 bridge, no upscaler) | Unchanged: `CapturedPresentGuides` still answers false every frame; the bridge's log line "using the upscaler's own depth and motion" never appears. |
| PCSX2 (`~/Games/pcsx2-nr-test/run.sh`, D3D12 present, no upscaler) | Unchanged: no capture, no ring, no decision. |
| DOOM Eternal (Vulkan) | Out of scope; confirm only that nothing here runs. |
| A D3D11 title with an upscaler through Dx11wDx12 | Needed to exercise the bridge's use of the ring. **None of our four titles is one**; see the open questions. |

Native Windows on Rafael's RTX 3060 for the D3D12 debug layer on the depth-copy formats is the
standing request in `nr-present-hook.md`; this change adds no new copy, so it neither adds nor
retires that request.

## Step 0 as built

The maintainer chose the log-only first slice (open question 3). What exists, all of it inert at the default
`HookMethod=1`, with the pass off, and in a title whose ring never records:

- **The stamp.** `CaptureTemporal` calls `StampPresentGuide` after both copies are recorded and right before
  `CommitRecord`, under `g_nrMutex`. It reads the `Output` resource's extent and `DLSS.Output.Subrect.Base.X/Y` from the
  evaluate's parameters, and takes the render extent, the device, the boundary count, the ordinal and the swapchain.
- **The decision header**, `retoque/dlssnr/DlssNr_PresentGuideIdentity.h`: `Decide` applies D2's eight rules in
  order to `PickForPresent`'s answer and returns that slot or none, with the first failing reason. `Tracker` keeps the
  stamps, the presenting-swapchain registry and the boundary count; `Consumer` is step 0's record.
- **Both consumers ask, and neither listens.** `RunPresentPass` calls `NotePresentPassIdentity` on the line after its
  pick. `CapturedPresentGuides` calls `NoteBridgeIdentity` there too; the bridge host now passes itself as its presenter
  and the frame's extent (new defaulted arguments), and is counted only with the pass on and at `HookMethod=2`, the
  only value the bridge is built for. Both return `void`; the slot each present reads is still `pickedSlot`, and
  `tests/nr-present-guide-identity` holds that by source guard.

Where it departs from D1 and D2 as written, and why:

1. **The boundary count is the identity layer's own**, not `g_presentFlip`. It advances once per present that
   reaches the pick, on either consumer, under `g_nrMutex`. `g_presentFlip` is written on the present thread outside
   the lock while captures read on the render thread, and the D3D11 bridge never advances it. Presents that return
   earlier (the four warm-up flips, an HDR decline, the wrapped entry standing down for the FG hook) mark no boundary.
   The captures made during such a run all share one count, so the first present after it can read `ambiguous` once.
2. **`evalsSincePresent` is counted since the last boundary**, not since the last consuming present: the capture's
   ordinal on its device at its output extent. That is rule 4's "all but one older than a frame boundary that a present
   marked", made countable at capture. A capture after the picked one and still in the ring is added at the decision.
   A fifth extent in one frame counts as unknown, which rule 4 reads as ambiguous.
3. **The output reads are in `CaptureTemporal`, not `GatherFrame`.** `GatherFrame` runs on every evaluate, at
   `HookMethod=1` too, so the reads go only where the capture is.
4. **Rule 8's render extent is the stated subrect, or else the depth texture's extent.** The pass reads the depth
   texture's extent when the game states no subrect (`DlssNrFrameInfo`: "zero means the game did not say"). Refusing
   zero literally would refuse every DLAA title without subrect keys. Half a subrect is refused.
5. **The registry counts presenters seen at the pick in the last 1000 ms, per device.** The D3D12 pass registers the
   swapchain it was handed; the bridge registers its `PresentHost`. A capture made before any present has no stamp, and
   it matches a present that is alone on its device.
6. **Rule 5 compares against the last capture this consumer read today.** Each consumer keeps its own record.

### The log line

One line per consumer per window, from `Consumer::Note`. A window lasts at least 2 s. While it uses the same buckets
as the last line written, it is held up to 30 s, with its counts carried over the whole interval; a change is written
at 2 s. Each consumer's first window is always written. A ring that never recorded logs nothing. Example (counts made
up):

```
DLSS-NR present guides, identity check (log only, nothing refused), D3D12 present: 120 presents in 2.0 s | today 118 read a capture, 2 skipped with no new capture, 0 had none | of those read, 112 would pass, 6 would be refused (lag 6) | skipped but would pass 0 | epoch lag of new captures 0:0 1:112 2:6 3:0 4+:0 | captures per frame at their extent 1:118 2:0 3+:0
```

- `D3D12 present` or `D3D11 bridge`: which consumer.
- `presents`: the presents that reached the pick in the window.
- `today`: what the present does now with the pick. It reads it; or it skips (`RunPresentPass`'s own skip, nothing
  recorded since its last pass, which is the generated-frame case; the bridge has no skip); or nothing was eligible,
  and zero or synthesized guides run.
- `of those read`: the decision over today's reads. Refusals are counted by first failing reason, the non-zero ones
  only, in rule order: `device`, `extent`, `output-base`, `swapchain`, `ambiguous`, `consumed`, `lag`, `stale`,
  `subrect`. `consumed` here means today re-read a capture an earlier present had already read. That happens when a
  newer capture exists but its copy has not completed.
- `skipped but would pass`: today's skip passed over a capture the rules accept. That is a capture recorded while
  the last pass ran, completed since.
- `epoch lag of new captures`: the present's boundary count minus the capture's, counted once per capture at the first
  present it is offered to. `1` means captured in the interval just before this present.
- `captures per frame at their extent`: rule 4's count for each newly offered capture (same device, same output
  extent, same boundary).
- `last extent refusal: output WxH at (X,Y), backbuffer WxH`: present only when the window had an `extent` or
  `output-base` refusal.

How to read it for step 1:

- **`kLag`.** Add the lag buckets over a session. Mass at `1` keeps `kLag = 1`. Mass at `2` means a render thread a
  frame ahead, or copies completing a frame late on another queue; in the second case `consumed` shows among the reads
  beside it. Then `kLag = 2`. If there is no small cutoff, rule 6 is dropped for that title (D2).
- **Rule 4.** Non-zero `2:` or `3+:` means the title evaluates more than once per frame at one extent, and
  `ambiguous` is the number of real-guide frames step 1 would take away from it. If those buckets stay zero, rule 4
  costs nothing there.
- **Rule 2.** `extent` or `output-base` outside a resize means the title scales, letterboxes or crops after the
  upscaler, or renders into part of a larger texture; the trailing field says by how much.
- **Device and swapchain.** Any `device` refusal is a stamp that disagrees on device, the last question step 0
  asks. `swapchain` refusals with no second window on screen mean the registry misreads which chain presents.
- **The design's own test.** If `would be refused` is a large share of `read` across a session that looks right on
  screen, the rules are wrong, and this note is rewritten before step 1.

Run the test plan's step 0 on it: Crimson Desert with DLSS-G off, `HookMethod=2` and `0`, through a menu, a cutscene,
a resolution change and Alt-Tab. Then `HookMethod=1`, where the line must not appear. The string
`identity check (log only` is in the DLL when the slice is linked in.

## Risks

- **`kLag` has no small constant in a title.** Then rule 6 is dropped for it and identity rests on
  rules 2 to 5. The risk is a title where that is not enough; step 0 finds it first.
- **Rule 4 refuses titles that evaluate twice at one extent.** They lose real guides at present and
  fall back to zero or synthesized motion. That is a visible regression against today for those titles
  (today they get the newest capture, right or wrong). Whether that trade is acceptable is the
  maintainer's call.
- **Asynchronous compute evaluates** lose frames to the no-GPU-wait rule. Visible in the log, not
  fixable here.
- **The stamp's swapchain field is a guess when more than one swapchain exists**, and a null stamp
  with two swapchains refuses. A title that creates a second swapchain it never presents to would
  lose real guides; the registry must count swapchains that have presented, not swapchains that exist.
- **HUD and post-upscale effects** are not guided by what was captured. Unchanged from
  `nr-present-hook.md`; named so the feature is not read as removing it.
- **NeuRotic's behaviour is prior art, not a specification.** Its identity record was designed for
  its own queue and Streamline handling; nothing in this tree has been checked against its
  behaviour in a running game.

## Open questions for the maintainer

1. Is refusing ambiguous frames (rule 4) acceptable, given that it removes real guides from any title
   that evaluates twice at one extent?
2. Should present refuse `MVJittered` as NeuRotic's D3D11 route does, or keep parity with the
   after-upscale route, which ignores the flag?
3. Is a log-only step 0 the right first slice, or should the stamp and the decision go in behind a
   key at once?
4. Which D3D11 title with a real upscaler is available for testing the bridge consumer of this
   ring? Without one, the bridge half of this change is host-tested only.
5. Should a refused frame under `RequireDlss=true` skip the pass (show the game's frame, as today), or
   is that wrong for the frames refused for identity reasons rather than absence?
