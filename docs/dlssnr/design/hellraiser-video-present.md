# Hellraiser: video coverage at the game's Present

2026-10-15; based on `a1b86054`. Investigation and a narrowly scoped correction, not a game acceptance result.

## Finding and evidence

Hellraiser's default `HookMethod=auto` resolves to **1**, not numeric Auto (0). `Stage=auto` resolves to 0.
`EvaluateAfterUpscale` in `retoque/shaders/dlssnr/DlssNr_Dx12.cpp` gets the NGX output resource and dispatches
in-place on it. It does not process media or UI drawn later. `RunPresentPass` reports the most recent
upscaler result on this route; that status is not proof of final-screen coverage, and can remain active
when no new evaluate arrives. The coverage log explicitly says `evidence=CPU-recorded-not-GPU-complete`.

Read-only samples of the installed `OptiScaler.log` and `OptiScaler.previous.log`, in
`Hellraiser/Binaries/Win64`, both identify build `a1b86054` (`20261015_103708`). At 13:31:57 the current
session had `calls=67260 model_ok=67138 model_failed=0 applied_recorded=67138 skipped=122 fallback=0`.
The previous session's last coverage window (13:05:46) had 19,275 successes, zero failures and 67 skips.
The sampled coverage reasons are composition recorded into upscaler output and resolution settling.
These counters contradict a lasting failure of the model on evaluated frames; they do not prove that
all displayed frames, or videos, include that composition. The logged off/on at 13:08:09–31
must be excluded from an automatic-stop diagnosis. **One logged off event is not proof of only one
toggle:** `ReportSkipOnce` suppresses each reason for the entire process, and the disabled branch
returns before constructing `CoverageScope`. A later F7/menu off can therefore be silent. The current file was growing during inspection.

The continued log adds a 198.618-second gap between 13:26:43.211 and 13:30:01.829: coverage calls
increase by only 22 while `present_id` increases from 123817 to 161350 (37,533). There are no status
lines in between either. This is compatible with a later disabled interval, and is not proof of a
movie or a stopped model. Nothing identifies the user's action there. The short 12.37-second gap
around the menu and isolated Streamline missing-depth/motion-tag errors (13:07:08 and 13:23:51)
likewise cannot be assigned to a movie without a timestamped reproduction. Model failures stay zero.

The media hypothesis is plausible, not established for this title. The installation contains 51
`.bk2` and 9 `.mp4` files under `Hellraiser/Content/Movies`; filenames establish available media,
not how any particular clip is composited. Epic's [Bink documentation](https://dev.epicgames.com/documentation/unreal-engine/bink-video-for-unreal-engine)
describes overlay playback after scene graphics and before UI, but also playback into render targets
that enter the scene normally. Its [Media Framework example](https://dev.epicgames.com/documentation/unreal-engine/play-a-video-stream-in-unreal-engine)
uses a fullscreen widget. Neither establishes Hellraiser's chosen path. A video drawn after the NR
input, or a separate movie swapchain, needs separate verification; a video on an in-world surface may
already pass through NR. No game assets, configuration or process state were modified.

## Existing route and the specific defect

`HookMethod=2` set **before process startup**, with NR enabled, installs `DlssNr_SlBoundaryProbe` on the
Streamline interposer. Its `Enter` calls `RunPresentPass(..., boundary=true)` before forwarding the
app's Present, on the game's native creation queue. The lower wrapped entry stands down while that
boundary is live. This ordering was measured in Crimson Desert, not yet in Hellraiser/Streamline 2.9.
If that boundary is unavailable while the game's DLSS-G is active, the lower entry declines rather
than modifying alternating real/generated frames. Changing the menu setting after startup cannot
install the missing boundary: restart is required.

A separate, reproducible code defect would defeat video playback even at this boundary: once a guide
capture has been consumed or permanently refused, an unchanged newest capture sequence makes every
subsequent present return. An upscaler that stops evaluating for a movie thus stops the present pass,
even with `RequireDlss=false`. Before any guide has ever arrived, the same host already runs without
game guides. This is not the cause of the supplied **HookMethod=1** session; it blocks its proposed remedy.

## Bounded change

Keep the no-new-capture skip on every lower/unknown present entry, and whenever `RequireDlss=true`.
At the actual app boundary only (`boundary`, never the recent-boundary liveness flag), with
`RequireDlss=false`, let the existing guide identity decision and fallback proceed. Consumed, stale,
lagged or incompatible guides remain rejected before any read. Missing guides use constant depth and
zero motion, or the existing synthesized-motion option. The existing four-present source transition
resets history once; `ZeroGuideReset=true` then resets zero-guide frames. New captures resume normal
identity checks and game-guide use. No new allocations, locks, waits, config keys, shader changes,
automatic route changes, or lower-entry permissions are introduced by this condition.

The app boundary proves this is an application present, not that pixels changed or were redrawn.
As with the existing never-upscaled fallback, a game that presents an unchanged already-edited buffer
can accumulate the effect. Held video frames must be checked for this. The guard is retained on lower
entries because absence of captures there cannot distinguish a generated frame from a movie frame.

Default HookMethod=1, disabled NR and guide-required playback retain their current paths.
The regression test executes the production admission predicate for first capture, consumed capture,
quiet periods, resumed captures, required guides and both entry types. Existing identity tests must
still prove consumed guides are never reused. Full host tests run in the capped scope; a game run is
needed to verify actual interposer ownership, GPU ordering, pixels and pacing.

## Proposed game experiment (Claude/Gabriel performs it)

With the game closed, replace the existing values in `[DlssNr]`, not duplicate them:

```ini
Enabled=true
HookMethod=2
RequireDlss=false
ZeroGuideReset=true
SynthMotion=false
GpuTiming=true
```

Start with DLSS-G off for image diagnosis, then repeat with it on from a fresh launch. Leave the
remaining colour/model settings unchanged. `Stage` does not choose the present route. For Streamline,
set HookMethod in the ini before launch even when FG is initially off. In the NR menu, Present is the
placement to inspect; selecting it live is not a substitute for restarting. To restore the old route,
use `HookMethod=1` (or the shipped `auto` value) and restart.

Require the log line:

```
DLSS-NR present: the pass runs at the game's own Present, ahead of its frame generation, on the game's queue (nr-before-dlssg.md, step 1)
```

Also check `DLSS-NR boundary probe:` windows: app presents, wrapped presents, native queues and
`DLSSG.Backbuffer` matches. After-upscale coverage should say `deferred to present-time hook`; present
guide windows should keep reporting frames with none/refused during upscaler silence, instead of only
`skipped with no new capture`. Existing source-change logs/status distinguish zero/synthesized/game
guides. A decline with `the game's DLSS-G interpolates before this pass` means this test did not reach
the required boundary. Do not remove that refusal.

Repeat the same gameplay location and movie segment, recording timestamps for every apparent loss
and F7 toggle. Compare a screenshot of the displayed frame to the NR original/processed capture
(menu's comparison capture); an after-upscale capture alone cannot show a video drawn later. Capture
both moving footage and a held/paused frame, transitions into/out of playback, subtitles, menus,
Alt-Tab and a resize. Collect GPU timing, base-frame rate and pacing with FG off/on, not just the
on-screen generated-frame rate. No capture was triggered in the running game by this investigation.

Risks: the final image includes HUD/subtitles and tone mapping; NR can alter text, faces, grain and
compression artifacts. Zero motion does not describe movie motion; per-frame reset trades ghosts for
possible flicker. SynthMotion is a second experiment, with constant depth and its own cost/edge errors.
If the game keeps evaluating DLSS behind a movie, identity by extent/frame epoch does not establish
that the guides describe the movie: the host may still accept background-scene guides. This fix does
not detect that case. DLSS-G's separate HUD-less tags can likewise refer to scene content; boundary
ordering alone does not prove all generated video frames contain the edit. No guarantees until A/B.

## Why not switch automatically

Quiet detection misses a fullscreen video over a still-rendered 3D scene. Pixel divergence also occurs
normally from tone mapping, UI, postprocessing and generated frames; it is not a reliable video flag.
A general solution needs an explicit content/guide policy, GPU-side comparisons of comparable images,
source history, single-route ownership, clean-frame provenance and verified FG ordering. Switching
model ownership on a threshold risks double processing and history churn. That is a separate design,
not a safe small fix. Prefer explicit placement and measured coverage first.
