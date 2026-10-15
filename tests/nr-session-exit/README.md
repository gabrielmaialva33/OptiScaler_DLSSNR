# NR session guard: the exit signal under Wine

```bash
python3 tests/nr-session-exit/run.py                 # the system wine and the newest GE-Proton found
python3 tests/nr-session-exit/run.py --wine /path/to/wine
```

Tier `wine`. The runner builds `harness.cpp` with the shipped `retoque/dlssnr/DlssNr_ProcessEnd.cpp` (the session
guard's hook on `ntdll!NtTerminateProcess`) through the msvc-wine toolchain, in a private copy of its prefix taken
while nothing else uses it, and links Detours from `OptiScaler/library/detours`. Then it runs the harness once per
way a process can end, under each Wine it finds, each in an isolated prefix under `artifacts/`. The harness records
every callback the hook makes in a file; the runner judges the file and the exit status. Design:
`docs/dlssnr/design/session-guard.md`, "The exit signal".

Never loads OptiScaler.dll, NGX or a game.

## What this covers

Each self-initiated end must reach the hook exactly once, with the status the note predicts, while a worker thread
is still running (`others=alive`: in `ExitProcess` the hook comes before `NtTerminateProcess(NULL)` stops the other
threads); the process exit status must match:

| Mode | How it ends | Recorded |
|---|---|---|
| `exit-process-0`, `exit-process-3` | `ExitProcess(n)` | `n` |
| `crt-exit-5` | `exit(5)`: atexit, then `ExitProcess` | 5 |
| `return-6` | return from `main` | 6 |
| `terminate-self-0`, `terminate-self-7` | `TerminateProcess(GetCurrentProcess(), n)`, no DLL detach | `n` |
| `terminate-self-handle-9` | `TerminateProcess` on a real handle to itself | 9 |
| `access-violation` | unhandled exception, default end (`SEM_NOGPFAULTERRORBOX` set: no debugger) | `0xC0000005` |
| `access-violation-default` | the same as a game has it: the AeDebug debugger (`winedbg --auto`) attaches first | `0xC0000005` |
| `abort` | the CRT's `abort()` (`__fastfail`) | nothing: ended below user mode, as on Windows (exit status `0xC0000409`) |
| `own-filter-exit-3` | an unhandled exception whose filter calls `TerminateProcess(self, 3)` | 3: the known gap, a crash handler's own code |
| `kill-other` | terminates a watched child, then `ExitProcess(0)` | 0, once; the child, killed from outside, records nothing |

Before the cases, each prefix gets `ShowCrashDialog=0` (as Proton's prefixes have it), so winedbg never waits on a
window; a case that still hangs is reported as a 120 s timeout, not a dead runner. The build prefix is copied once;
delete `artifacts/` to take it again after a toolchain change.

The hook is also asked twice in every mode (`Watch` returns the first answer and attaches nothing more), and the
first-call rule is what keeps `ExitProcess`'s second `NtTerminateProcess` (after the DLL detach) from recording again.

## What this does not cover

The guard's decisions (`tests/nr-session-guard`), the marker file's I/O, and any game: whether DOOM Eternal and
Crimson Desert end themselves or are ended from outside is for the game run in the design note's "Before the default
changes". Native Windows is not run; the Windows rows in the note come from the documented behaviour of
`RtlExitUserProcess`, `TerminateProcess` and the unhandled-exception path.

## Results

2026-10-15, first run: 22 of 22 (11 ends under each of wine-11.19 and GE-Proton11-7's wine-11.0 Staging). Every
self-initiated end recorded once with `others=alive`; the killed child recorded nothing. `abort` was expected to
record `0xC0000409` in the first draft and recorded nothing: Wine handles `__fastfail` below user mode, like
Windows. The expectation and the design note were corrected; for the guard that end is "no end seen".

After review, `access-violation-default` was added (a game does not set `SEM_NOGPFAULTERRORBOX`): under wine-11.19
winedbg attached and printed its backtrace, and the process still ended through the hook with `0xC0000005`;
GE-Proton's recorded the same. 24 of 24.
