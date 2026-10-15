# NR session guard

```bash
python3 tests/nr-session-guard/run.py
```

The runner compiles `retoque/dlssnr/DlssNr_SessionGuard.h`, the header-only decision unit, with g++ C++20,
ASan/UBSan and `-Werror`, and drives it with a fake environment (file, process table, clock, exit watch). Design:
`docs/dlssnr/design/session-guard.md`.

## What this covers

- **The policy table, by how the last session ended.** A normal quit (the process ends itself with status 0) leaves
  no marker and no verdict, however short the session; an exception status or a device NR saw lost disables under
  10 minutes and warns after, the second in a row disabling; a nonzero exit code and "no end seen" disable under 2
  minutes, warn up to 10 (the second consecutive one disables) and are not blamed after; the boundaries of each and
  of the exception class (`0x7FFFFFFF` an exit code, `0x80000000` and `exit(-1)` exceptions).
- **The end of the process.** The record written then (ended, code, the time to the end) and read back without a
  probe; a failed delete at a normal quit becomes an inactive marker; nothing written for a guard that never ran;
  after the end no thread starts a marker; a lock held by a blocked heartbeat is retried a bounded number of times
  and then left (nothing written, never a hang).
- **Device removal.** Written at once and once; kept through a normal quit; named on the next launch whether the
  process then recorded an end or was killed.
- **Who owns the marker.** A live second instance (same pid and creation time) and a probe that cannot tell are
  left alone and write nothing, not even at the end; a reused pid (different creation time) is stale; the probe's
  answers mapped (`ERROR_INVALID_PARAMETER` dead, other failures unknown, a finished process dead).
- **What cannot be read or written.** Corrupt, truncated, inconsistent, version 1 and future-version markers count
  as no marker; a read-only directory produces no verdict (an unpersistable verdict would repeat forever), no retries,
  and nothing at the end; an exit watch that cannot be put in place means no marker at all.
- **Switched off.** `SessionGuard=false`, or NR off at start: nothing read, written or removed, no watch.
- **The marker's life.** Written at the first success, heartbeat at most every 30 s, removed on a clean off, new
  marker after a toggle (with its own removal state); "Enable anyway" clears block and strikes; the block is the only
  thing a consumed marker shows again.
- **The words.** The menu's reason line for every verdict the policy produces and the log's account of the end, exact.
- **Source guards.** The call sites exist (start, detach before the `lpReserved` break, both evaluate routes, the off
  paths, the two device-removal notes inside the module and none upstream); the hook targets
  `ntdll!NtTerminateProcess`, recognises this process three ways, acts once and passes every call through; the code
  that runs at the end does not log, allocate or lock; the disable goes through `set_volatile_value`; the new files
  are in the project; every reason line and the menu's number formats are in the pt-BR pack and the design note.

## What this does not cover

The Windows layer (`DlssNr_Session.cpp`): the file replace, `OpenProcess`/`GetProcessTimes`, the config write, the
log calls and the menu. The hook itself in a real process is `tests/nr-session-exit` (wine tier). Whether a game's
quit, a crash or a Steam "Stop" ends the way the note predicts is for a game run to show.
