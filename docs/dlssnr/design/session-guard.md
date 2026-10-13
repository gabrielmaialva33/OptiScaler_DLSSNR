# Session guard — do not crash the next launch the same way

Status: design, then implement (design-doc-first per DEVELOPMENT.md). D3D12 and native Vulkan share it: the
guard sits at the two places an NR evaluate succeeds, not inside either route.

## The problem

NR runs an undocumented model inside the game's process. When a session ends in a crash, a device removal or a hang
the user has to kill, the next launch loads the same settings and usually does the same thing: a crash loop the
user has to break by editing the ini, if they know which key. Prior art is NeuRotic's `NrExperimentalSession.h`
(GPL-3.0, read for the idea only, nothing copied): a hidden state file holds {active, pid, process creation time};
on the next start, an `active` marker whose process is gone means the previous session did not finish, so the
experimental overrides are switched off and the user is told.

The idea is sound and the naive form of it is harmful. "Active marker, process gone" is also what every
non-crash exit looks like when the game never reaches `DLL_PROCESS_DETACH`.

## What can and cannot be detected

OptiScaler sees an exit cleanly only through `DLL_PROCESS_DETACH` with `lpReserved != NULL`, which runs inside
`ExitProcess`. There is no `ExitProcess` or `TerminateProcess` hook in this tree, and one would not help: the
cases that matter never call either.

| End of the session | Marker cleared? | Honest classification |
|---|---|---|
| Game quits through `ExitProcess` (menu quit, alt-F4 that the game handles) | yes, at detach | clean |
| NR switched off at runtime, or the model's own fault latch fired | yes, immediately | clean: NR was not running |
| Steam "Stop", `TerminateProcess`, Proton killing the prefix | no | **indistinguishable from a crash** |
| Hang the user kills | no | indistinguishable, and a real fault |
| Crash in the game's own shutdown after `ExitProcess` began | detach may still run | clean, correctly: NR had finished |
| Crash in the driver or model during play | no | unclean, the case to catch |
| Device removal that the game survives | n/a | not a session end; the model's fault latch covers what it can |

Candidates for a "clean" signal and why they were not taken:

- **Swapchain release.** It also happens on every resize, fullscreen toggle and device recreation; NR resumes
  after those, so it would clear a marker mid-session, and a crash right after would go unseen.
- **Window destroy / `WM_CLOSE`.** Games reach it on some exits and not on others, and `WM_CLOSE` is a request, not
  an end. It adds a hook for a signal weaker than detach.
- **An `ExitProcess` hook.** Same coverage as `DLL_PROCESS_DETACH` (both run only for orderly exits), plus a new
  hook that would have to survive every proxy-name disguise. No gain.

So the only way to cut false positives is not to treat every unclean end as a crash. The one piece of extra
evidence a marker can carry is how long NR had been working: a model that faults does so early (first frames, a
size change, a content change) far more often than after ten quiet minutes. A heartbeat in the marker records that.

## Policy

An NR session is **active** from its first successful evaluate. The marker records the first and the last beat; the
last beat is rewritten at most every 30 s while NR evaluates. On the next start with NR enabled, a stale marker
(see below) is classified by `ran = lastBeat - first`:

| Previous session | Action |
|---|---|
| No marker, or `active=0` | normal start |
| Unclean, `ran` under 120 s (**early**: a crash loop at start) | **disable NR for this session**, and keep it disabled on later launches until the user says otherwise |
| Unclean, 120 s to 10 min (**middle**) | notice only; counts as one strike |
| Unclean, second consecutive strike | disable, as for early |
| Unclean, 10 min or more (**long**) | log line only, no notice, strikes reset: NR demonstrably worked, and the end was far more likely the game's or the user's |
| Marker from a live second instance | ignore it entirely; this instance writes nothing |
| Marker unreadable, corrupt or for another version | treat as no marker; never penalise what cannot be read |
| Directory not writable | guard inert: no verdict, no file |

Why one strike is enough for an early end and why it stays blocked: the cost of a false positive is one click
("Enable anyway"); the cost of a false negative is the crash again, and an every-other-launch protection (disable
once, then try again) leaves a start-up crash loop half unprotected. A block therefore persists in the marker
(`blocked=1`) until acknowledged or until an enabled session ends clean. Why the long exemption: Steam "Stop" after
an evening's play must not cost the user NR the next time, and the evidence says NR was not the problem.

What this still gets wrong, stated plainly: a user who stops the game from Steam within 2 minutes, twice, sees a
block they did not earn, and a model fault after 10 minutes of play is reported as nothing. Both are accepted; the
first is one click, the second is the model fault latch's job, which already works in-process.

## What "NR active" means

Set when an NR evaluate returns success (`result == 1`, after the model call in `DlssNr_Dx12.cpp`, after the
evaluate in `DlssNrFeature_Vk.cpp`), not when `Enabled=true`: a game that never reaches an upscaler, or a model
that fails to build, has run nothing that could have crashed. Cleared (the file removed) when:

1. NR is turned off at runtime (menu, toggle key): the three `!enabled` entry points call `NoteOff()`;
2. the model's fault latch trips (`NoteModelFault`): NR is off for the session already;
3. `DLL_PROCESS_DETACH` with `lpReserved != NULL`, the one orderly exit there is.

## The marker

`retoque_nr_session.state` beside the log (`Util::DllPath().parent_path()`), plain text, one `key=value` per line
under a version line:

```
retoque-nr-session 1
active=1
pid=4242
created=133790000000000000   process creation time, FILETIME ticks
first=1760000000             unix seconds of the first NR success
beat=1760000620              unix seconds of the last heartbeat
streak=1                     consecutive unclean sessions so far
blocked=0
```

Writes go to a temporary sibling and replace the marker in one step, falling back to a direct write; any failure
is swallowed (debug log once). The game never learns the file exists. Reads accept only the exact version line and
well-formed values (`beat >= first`, nothing negative); anything else is "no marker".

**Stale or live.** `active=1` means a session was running when the file was last written. The probe asks whether
`pid` is alive and, if so, whether its creation time equals `created`:

- dead: stale, classify it;
- alive, same creation time: a second instance of the game is running right now; leave its marker alone and write
  none (two instances sharing one directory cannot each own the file);
- alive, different creation time: the pid was reused by an unrelated process; stale;
- cannot tell (access denied, no creation time): treated as live. An unknown never produces a verdict.

On Wine each prefix has its own pid space; two prefixes sharing one directory is outside what the pid can tell
apart and is accepted.

The verdict is **persisted before it is applied**: the marker is rewritten as `active=0` (with `streak`, `blocked`)
and, if that write fails, there is no verdict. A directory that cannot be written would otherwise present the same
stale marker every launch and disable NR forever.

## What the user sees

In the NR card, under the enable checkbox, when a verdict exists:

- **Disabled:** a warning line, e.g. "Neural Rendering was switched off for this session: the last session ended
  without closing 2 minutes after the model started." with an **Enable anyway** button. The button clears the
  block and the strikes, turns NR on for now, and the user's ini is untouched either way.
- **Notice:** one line, no button, NR stays on.

One `WARN` log line at start states the verdict and the numbers (`ran`, `streak`). The disable uses
`DlssNrEnabled.set_volatile_value(false)`, the existing "do not save this" assignment, so `SaveIni` keeps writing
what the user had; the menu checkbox simply shows it off. Never rewrites the ini.

## Config

`[DlssNr] SessionGuard` (bool, default true): false turns the whole thing off: no read, no write, no notice, the
file left as it is. Four-point round trip: `Config.h`, read and save in `Config.cpp`, `OptiScaler.ini`, and the
`Config.md` entry.

## Guards (per DEVELOPMENT.md)

- Default-identical when NR is off: with `Enabled` false at start, `Begin()` reads and writes nothing and no
  notice exists. A runtime turn-on starts the marker at the first success, without a verdict for older markers.
- No control for a thing that does not exist: the notice and the button exist only while a verdict does.
- The per-frame cost is one atomic load when no marker is owned, and one uncontended lock, a clock read and a comparison when one is.
- Decision logic is `retoque/dlssnr/DlssNr_SessionGuard.h`, header-only, no Windows types, tested by
  `tests/nr-session-guard/` against a fake environment. The Windows layer (`DlssNr_SessionGuard.cpp`) is file
  I/O, `OpenProcess`/`GetProcessTimes` and the config write, and is not exercised on the host.
- Call sites outside the module are one-liners: `dllmain.cpp` (`Begin` after Wine detection, `NoteExit` at detach).

## Not done

- No clean-exit signal beyond detach (table above).
- No separate rule for a hang versus a crash.
- A "never ask again" persistence: `SessionGuard=false` is that control.
