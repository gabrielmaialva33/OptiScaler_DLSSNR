# Session guard — do not crash the next launch the same way

Status: version 2 (2026-10-14) adds the exit signal version 1 lacked: the process's own end, seen inside the
process by a hook on `NtTerminateProcess`. Its behaviour under Wine 11.19 and GE-Proton11-7's own Wine is measured
by `tests/nr-session-exit` (see "Measured under Wine"). **It has not run in a game yet**, so `[DlssNr] SessionGuard` stays off by default until
the check in "Before the default changes" is done. D3D12 and native Vulkan share the guard: it sits at the places an
NR evaluate succeeds, not inside either route.

## Measured (2026-10-13, build f56379c9, Proton) — why version 1 is off

Two ordinary quits from the game's own menu, and both left the marker behind with `active=1`:

| Title | NR before the quit | Marker after | Next launch would have |
|---|---|---|---|
| DOOM Eternal (Vulkan) | about 90 s | present | **disabled NR** (early end, under 120 s) |
| Crimson Desert (D3D12, DLSS-G) | about 4 min | present | shown the "middle end" notice |

So these exits never ran `DLL_PROCESS_DETACH` (or never reached `SessionGuardOffAtExit` in it): version 1 assumed a
menu quit goes through `ExitProcess`, and for these two engines under Proton it does not. Whether the engine calls
`TerminateProcess` on itself or Proton ends the process was not settled; either way every short session read as a
crash, which is the false positive the policy was built to avoid. (The log is no help here: upstream writes its
`DLL_PROCESS_DETACH` line only on an explicit `FreeLibrary`, so its absence proves nothing.)

Version 2 does not need to settle which one it was, as long as it is one of the two. Wine's loader does deliver
`DLL_PROCESS_DETACH` at `ExitProcess` to a DLL that called `DisableThreadLibraryCalls` (`process_detach` skips only
modules not attached, `dlls/ntdll/loader.c`), so the likely reading is that both engines end themselves with
`TerminateProcess`. If instead something outside the process ended them, no in-process signal can tell that from a
Steam "Stop", and the guard is not for those titles; the check below shows which.

## The problem

NR runs an undocumented model inside the game's process. When a session ends in a crash, a device removal or a hang
the user has to kill, the next launch loads the same settings and usually does the same thing: a crash loop the
user has to break by editing the ini, if they know which key. Prior art is NeuRotic's `NrExperimentalSession.h`
(GPL-3.0, read for the idea only, nothing copied): a hidden state file holds {active, pid, process creation time};
on the next start, an `active` marker whose process is gone means the previous session did not finish, so the
experimental overrides are switched off and the user is told.

The idea is sound and the naive form of it is harmful. "Active marker, process gone" is also what every
non-crash exit looks like when the game never reaches `DLL_PROCESS_DETACH`, and version 1 measured exactly that.

## The exit signal (version 2)

Every end a process brings on itself passes through `ntdll!NtTerminateProcess` **inside that process**, on the
thread that ends it, before the end happens:

| How the session ends | What runs in the dying process | Seen as |
|---|---|---|
| `ExitProcess` / `exit()` / return from `main` | `RtlExitUserProcess`: `NtTerminateProcess(NULL, code)` stops every other thread, then the DLL detach, then `NtTerminateProcess(self, code)` | **ended itself**, with `code` (the first call) |
| `TerminateProcess(GetCurrentProcess(), code)` (engines that skip their own teardown) | `NtTerminateProcess(-1, code)`; no DLL detach at all | **ended itself**, with `code` |
| Unhandled exception, default handling | the thread-start filter ends it: `NtTerminateProcess(-1, exception code)` (Wine `call_unhandled_exception_handler`; Windows the same after WER) | **ended itself**, with the exception code |
| The game's own crash handler ends it | whatever it calls: its own `TerminateProcess(self, n)` or `ExitProcess(n)` | ended itself with `n` (see "What this still gets wrong") |
| Steam "Stop", Proton or wineserver killing the prefix, a hang the user kills, a crash handler process killing it, OOM or `SIGKILL` | nothing: the call is made in another process, or no code runs at all | **no end seen** |
| Fail-fast: `__fastfail`, which the CRT's `abort()` and `std::terminate` use; the kernel's stack-overflow kill | nothing in user mode, on Windows and on Wine alike (measured) | no end seen |

So the end the guard records is a fact about the process, not a guess from a duration: an end with status 0 is a
normal quit and clears the marker whatever path the engine took; an end with an exception status is a crash; an end
with another status is a failure exit; and no end at all means the process was stopped from outside, hung and was
killed, or died where no code runs. The hook is one Detours attach on `NtTerminateProcess` (`DlssNr_ProcessEnd.cpp`),
which acts only when the handle names this process (`NULL`, the pseudo-handle, or a handle whose `GetProcessId` is
ours) and only on the first such call, then passes everything through unchanged.

Status classes, from the NTSTATUS severity bits: **0** is a normal quit; **0x80000000 and above** (warning and
error severities: `0xC0000005` access violation, `0xC000001D` illegal instruction, `0x80000003` breakpoint, a crash
reporter's own `0xFFFF....`) is an exception, read as a crash; **anything else** is the program's own exit code.
An MSVC program's unhandled C++ exception goes to `std::terminate` and so to fail-fast: no end seen, not a recorded
code. `exit(-1)` lands in the crash class;
that is accepted, since it is not a normal quit either.

Candidates that were considered for the signal and not taken:

- **Swapchain release.** It also happens on every resize, fullscreen toggle and device recreation; NR resumes
  after those, so it would clear a marker mid-session, and a crash right after would go unseen.
- **Window destroy / `WM_CLOSE`.** Games reach it on some exits and not on others, and `WM_CLOSE` is a request, not
  an end.
- **Hooks on `kernel32!ExitProcess` and `kernelbase!TerminateProcess`.** Two hooks for less coverage: the default
  end of an unhandled exception calls `NtTerminateProcess` directly, and so do some engines and crash reporters.
- **`DLL_PROCESS_DETACH` alone.** Version 1, measured above. It stays as a fallback (below).

### Device removal as evidence

A GPU fault caused by the model typically shows up as a removed D3D12 device or a lost Vulkan device, and the game's
reaction varies: a crash (recorded as one), a hang the user kills (no end seen), or an error box and a quit, possibly
with exit code 0. So when NR itself sees the device gone while it owns a marker, the marker is rewritten at once
with `removed=1`, and a removal outranks whatever end follows. Two places, both in the module: the D3D12 evaluate's
`GetDeviceRemovedReason` check, which already runs before every NR evaluate (`DlssNr_Dx12.cpp`), and the Vulkan
present host's `MarkLost`. The Vulkan after-upscale route has no device-lost check of its own and adds none here.

## States

```
Idle     nothing to do, nothing to write: guard off, a live instance owns the marker, or a write or the hook failed
Armed    may start a marker at the next NR success
Running  owns an active marker: a clean end must clear it
Ended    the process is ending itself; nothing more is written, whatever thread asks
```

| From | Event | Action | To |
|---|---|---|---|
| (start) | `SessionGuard=false` | nothing read or written | Idle |
| (start) | NR off at start | nothing read or written | Armed |
| (start) | no marker, unreadable marker, or an inactive one | (a standing block is re-shown) | Armed |
| (start) | active marker, no recorded end, its process alive with the same creation time, or the probe cannot tell | write nothing, ever | Idle |
| (start) | stale active marker | classify (Policy), persist the verdict, then apply it | Armed |
| (start) | stale marker, verdict cannot be written | no verdict | Idle |
| Armed | first NR success | hook `NtTerminateProcess` (once per process), then write the marker | Running |
| Armed | hook does not attach, or the marker cannot be written | one info line | Idle |
| Running | NR success | heartbeat at most every 30 s | Running |
| Running | NR saw its device removed or lost | rewrite with `removed=1`, once | Running |
| Running | NR turned off, or the model's fault latch | remove the marker | Armed |
| Running | the process ends itself, status 0, no removal | delete the marker (if that fails: write an inactive one) | Ended |
| Running | the process ends itself, otherwise | rewrite: `ended=1`, `code`, last beat = now | Ended |
| Armed, Idle | the process ends itself | nothing written | Ended |
| Running | `DLL_PROCESS_DETACH` at exit and the hook never fired (fallback) | delete the marker, as version 1 | Armed |

`Ended` is what keeps a render thread from starting a fresh marker between the hook and the moment
`NtTerminateProcess(NULL)` stops it.

## Policy

On the next start with NR enabled, a stale marker is classified by how it ended and by `ran = beat - first`:

| How it ended | ran under 120 s | 120 s to 10 min | 10 min or more |
|---|---|---|---|
| device removed or lost | **disable** | **disable** | notice, a strike |
| crashed (exception status) | **disable** | **disable** | notice, a strike |
| exit code other than 0 | **disable** | notice, a strike | log line only, strikes reset |
| no end seen | **disable** | notice, a strike | log line only, strikes reset |

A strike that makes two in a row disables (`RepeatedEnd`). A disable persists in the marker (`blocked=1`) until
"Enable anyway", or until NR runs again in a session (turning it back on by hand is the same statement). A live
second instance's marker is left alone; an unreadable marker, or one in another format version (version 1's
included), counts as none; a directory that cannot be written produces no verdict.

Why the two evidence rows disable in the middle band and version 1's rows do not: version 1's middle rule existed
because "unclean" was not "crashed". A recorded exception or a device NR saw removed is a crash with the model
running, and the cost of a false positive is still one click. Why the exit-code row is treated like "no end seen"
rather than as clean: a nonzero exit is not a normal quit for nearly every game, it is how many crash handlers end
the process (Unreal Engine ends with 3 after its crash report), and the recorded code shows in the log and the menu,
so a game that does quit normally with a nonzero code is visible as such. Why the long exemption stays for those two
rows: Steam "Stop" after an evening's play must not cost the user NR the next time.

## What this still gets wrong

- **A crash handler that ends the process with exit code 0** reads as a normal quit. Exit codes 1-0x7FFFFFFF are
  caught by the exit-code row, which is softer after 2 minutes.
- **A game that quits normally with a nonzero code** is treated as a failure exit: a disable after a session under
  2 minutes. The code is in the log line, which is how it would be found.
- **Steam "Stop" within 2 minutes** disables, as in version 1: one click.
- **A device removal the game survives and NR does not see** (the Vulkan after-upscale route, or a game that quits
  before the next NR evaluate) is not evidence.
- **Contention at the end.** The end takes the guard's lock with a few short retries and never waits longer. If a
  heartbeat write was in flight on another thread at that instant (a window of about a millisecond every 30 s),
  nothing is written and the marker reads as "no end seen". The exit is never delayed by more than a few
  milliseconds for it.
- **A handle to itself opened without query access**, used to terminate itself, is not recognised (`GetProcessId`
  fails on it); the end then reads as "no end seen". No engine is known to do this.
- **Two Wine prefixes sharing one game directory** cannot be told apart by pid; accepted, as in version 1.
- **Under Proton with a debugger configured** (winedbg as AeDebug), an unhandled exception may be ended by the
  debugger from outside; it then reads as "no end seen", which is still unclean.
- **A hook on an ntdll syscall stub** is the kind of thing anti-cheat integrity checks look at. OptiScaler is not for
  anti-cheat titles in the first place, and this hook exists only with the guard on, in a session where NR ran.

## The marker

`retoque_nr_session.state` beside the log (`Util::DllPath().parent_path()`), plain text, one `key=value` per line
under a version line:

```
retoque-nr-session 2
active=1
pid=4242
created=133790000000000000   process creation time, FILETIME ticks
first=1760000000             unix seconds of the first NR success
beat=1760000620              unix seconds of the last heartbeat, or of the end
streak=1                     consecutive unclean sessions so far
blocked=0
removed=0                    1 once NR saw its device removed or lost
ended=0                      1 once the process recorded its own end
code=0                       the status it ended with; 0 unless ended=1
```

Reads accept only the exact version line, every key once, well-formed values, `beat >= first`, and three
consistency rules: `ended=1` only on an active marker; `code=0` unless `ended=1`; `ended=1` with `code=0` only with
`removed=1` (a plain normal quit deletes the file instead). Anything else is "no marker".

Writes go to a temporary sibling (`.tmp`) and replace the marker in one step with `MoveFileExW(..., REPLACE_EXISTING)`,
falling back to a direct write; Parse rejects a half-written file. Under Wine the replace is a `rename(2)` on the
Unix side, atomic on the same filesystem. Any failure is swallowed (one info line at runtime, none at the end).

At the end, inside `NtTerminateProcess`: no logging, no heap allocation of the guard's own (the text is built in a
256-byte stack buffer, the paths were made at start), and no lock waited on beyond a few 1 ms retries. In the
`ExitProcess` path the hook runs before any other thread is stopped, so a heap or file lock held by another thread is
released normally; the caller holds the PEB lock, which file calls on the same thread take recursively.

**Stale or live.** A marker with `ended=1` is stale by construction: the process said it was ending. Otherwise the
probe asks whether `pid` is alive and, if so, whether its creation time equals `created`:

- dead (`OpenProcess` fails with `ERROR_INVALID_PARAMETER`, Wine's `STATUS_INVALID_CID`; or the exit code is no
  longer `STILL_ACTIVE`): stale;
- alive, same creation time: a second instance of the game is running right now; leave its marker alone and write
  none;
- alive, different creation time: the pid was reused; stale. Under Proton this is the common case, not the rare
  one: each launch starts a new wineserver whose pid table starts over, so the game often gets the same small pid as
  last time. The creation time is what tells them apart;
- cannot tell (access denied, no creation time): treated as live. An unknown never produces a verdict.

The verdict is **persisted before it is applied**: the marker is rewritten as `active=0` (with `streak`, `blocked`)
and, if that write fails, there is no verdict. A directory that cannot be written would otherwise present the same
stale marker every launch and disable NR forever. A read-only game directory therefore costs the guard, never the
game: no verdict, no marker, one info line.

## What the user sees

In the NR card, under the enable checkbox, while a verdict stands:

- **Disabled:** "Neural Rendering is off for this session. Your settings are unchanged.", the reason, a dimmed line
  with the numbers, and an **Enable anyway** button. The button clears the block and the strikes and turns NR on
  for now; the ini is untouched either way.
- **Notice:** the reason and the numbers, no button; NR stays on.

| How it ended | Reason line | Numbers line |
|---|---|---|
| no end seen, under 2 min | The last session ended without closing, soon after the model started. | The model had run for N s. |
| no end seen, middle | The last session ended without closing. If it was a crash, this one may too. | The model had run for N s. |
| exit code, under 2 min | The last session ended with an error soon after the model started. | Exit code N after the model had run for N s. |
| exit code, middle | The last session ended with an error. If it was a crash, this one may too. | Exit code N after the model had run for N s. |
| crashed, under 10 min | The last session crashed while the model was running. | Exception 0xC0000005 after the model had run for N s. |
| crashed, 10 min or more | The last session crashed, long after the model started. If the next one does too, Neural Rendering will be switched off. | as above |
| device lost, under 10 min | The graphics device was lost while the model was running in the last session. | The model had run for N s. |
| device lost, 10 min or more | The graphics device was lost in the last session, long after the model started. If it happens again, Neural Rendering will be switched off. | The model had run for N s. |
| second strike in a row | The last two sessions ended without closing. | as for how the last one ended |
| a block still standing | An earlier session ended without closing, and this has not been confirmed since. | none |

One `WARN` log line at start states the verdict, how the session ended and the numbers, e.g.
`DLSS-NR session guard: The last session crashed while the model was running. (it ended with exception 0xC0000005;
ran 95 s, 1 unclean in a row); Neural Rendering is off for this session (the ini is unchanged)`. A long end that is
not blamed gets one `INFO` line. The disable uses `DlssNrEnabled.set_volatile_value(false)`, the existing "do not
save this" assignment, so `SaveIni` keeps writing what the user had. Never rewrites the ini.

**How to clear it:** "Enable anyway", or turn NR on with the checkbox or the toggle key (the first NR success clears
the block), or delete `retoque_nr_session.state`, or set `SessionGuard=false`.

## Config

`[DlssNr] SessionGuard` (bool, default false): false turns the whole thing off: no read, no write, no hook, no
notice, the file left as it is. Version 2 adds no key: the hook is part of the guard, not a separate choice, and a
hook that cannot attach turns the guard off for the session rather than falling back to version 1. Four-point round
trip as before: `Config.h`, read and save in `Config.cpp`, `OptiScaler.ini`, and the `Config.md` entry.

## Before the default changes

The default flips to true only after a game run shows the exit signal works where version 1 failed. With
`SessionGuard=true`, NR on, `LogLevel=2`:

1. DOOM Eternal and Crimson Desert, NR running for about a minute each, quit from the game's menu. Afterwards
   `retoque_nr_session.state` must be **absent** (a normal quit, status 0). If it is present with `ended=1`, the
   `code=` line is the game's exit code, and the policy's exit-code row decides whether that title needs a rule. If
   it is present with `ended=0`, the process was ended from outside and the guard cannot serve that title.
2. One game stopped from Steam ("Stop") within a minute: the next launch must show the "ended without closing" notice
   with the button, and the log's `WARN` line.
3. Restart once more without touching anything: "An earlier session ended without closing..." (the block stands);
   press "Enable anyway": NR runs, the next normal quit leaves no file.

## Guards (per DEVELOPMENT.md)

- Default-identical when off: with `SessionGuard` false, or `Enabled` false at start, nothing is read or written and
  no hook is installed; with NR turned on later, the hook and the marker start at the first NR success. The hook is
  installed only in a session where NR actually ran and the marker could be written.
- No control for a thing that does not exist: the notice, the numbers line and the button exist only while a verdict
  does.
- The per-frame cost is one atomic load when no marker is owned, and one uncontended lock, a clock read and a
  comparison when one is. The hook costs a pointer comparison on each `NtTerminateProcess` call, plus `GetProcessId`
  for a real handle.
- Decision logic is `retoque/dlssnr/DlssNr_SessionGuard.h`, header-only, no Windows types, tested by
  `tests/nr-session-guard/` against a fake environment. The hook is `DlssNr_ProcessEnd.h/.cpp`, Windows and Detours
  only, built as it ships by `tests/nr-session-exit/` under Wine. The glue (`DlssNr_Session.cpp`: file I/O,
  `OpenProcess`/`GetProcessTimes`, the config write, the log lines) is not exercised off a game.
- Call sites outside the module are one-liners and unchanged: `dllmain.cpp` (`SessionGuardBegin` after Wine
  detection, `SessionGuardOffAtExit` at detach, now a fallback). The device-removal notes are inside the module.

## Measured under Wine

`tests/nr-session-exit` builds the shipped `DlssNr_ProcessEnd.cpp` into a small program and ends it every way the
table above lists, under the system Wine (11.19) and under GE-Proton11-7's own (11.0 Staging, the build the test
games run on), reading what the hook saw from a file it writes. First run, 2026-10-15, 22 of 22:

- `ExitProcess(0)` and `(3)`, `exit(5)`, return 6 from `main`, `TerminateProcess` on itself with 0 and 7 through the
  pseudo-handle and 9 through a real handle, an unhandled access violation (`0xC0000005`), and a crash filter that
  ends the process with 3: each reached the hook exactly once, with that status, while a worker thread was still
  running. So in `ExitProcess` the hook runs before `NtTerminateProcess(NULL)` stops the other threads, and the
  second call after the DLL detach is not recorded again.
- A watched child killed from outside by its parent recorded nothing; the parent's own termination of that child
  was not taken for its own end; its own `ExitProcess(0)` was recorded once.
- `abort()` recorded nothing and the process ended with `0xC0000409`: Wine ends a fail-fast below user mode, as
  Windows does. The first draft of this note expected a recorded `0xC0000409`; the table above is corrected. For the
  guard it is "no end seen", which is unclean, so nothing changes in the policy.

The build used `/MT` and Detours from `OptiScaler/library/detours`, the same library the DLL links.

## Not done

- No rule per engine for exit codes; the code is shown so a rule can be argued from evidence later.
- No vectored exception handler to catch crashes a game's own handler turns into exit code 0: first-chance access
  violations are handled on purpose by some engines and protection layers, so "an exception shortly before a normal
  quit" would be a new source of false positives.
- No separate rule for a hang versus a crash: both end as "no end seen".
- A "never ask again" persistence: `SessionGuard=false` is that control.
