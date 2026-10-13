# NR session guard

```bash
python3 tests/nr-session-guard/run.py
```

The runner compiles `retoque/dlssnr/DlssNr_SessionGuard.h`, the header-only decision unit, with g++ C++20,
ASan/UBSan and `-Werror`, and drives it with a fake environment (file, process table, clock). Design:
`docs/dlssnr/design/session-guard.md`.

## What this covers

- **The policy table.** Clean previous session; unclean early (under 120 s: disable, persisted as blocked, still
  blocked on the next launch); middle (a notice, then disable on the second consecutive one); long (10 minutes or
  more: not blamed, strikes reset); the boundaries of each.
- **Who owns the marker.** A live second instance (same pid and creation time) and a probe that cannot tell are
  left alone and write nothing; a reused pid (different creation time) is stale.
- **What cannot be read or written.** Corrupt, truncated, wrong-version or inconsistent markers count as no marker; a
  read-only directory produces no verdict (an unpersistable verdict would repeat forever) and no retries.
- **Switched off.** `SessionGuard=false`, or NR off at start: nothing read, written or removed.
- **The marker's life.** Written at the first success, heartbeat at most every 30 s, removed on a clean off, new
  marker after a toggle; "Enable anyway" clears block and strikes.
- **Source guards.** The call sites exist (start, orderly detach before the `lpReserved` break, both evaluate
  routes, the off paths), and the disable goes through `set_volatile_value`, never an assignment `SaveIni` would write.

## What this does not cover

The Windows layer (`DlssNr_Session.cpp`): the file replace, `OpenProcess`/`GetProcessTimes`, the config write, and
the menu. Whether a real crash or a Steam "Stop" leaves the marker behind is for a game run to show.
