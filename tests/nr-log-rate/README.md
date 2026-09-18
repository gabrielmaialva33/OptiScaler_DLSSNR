# NR log rate limiting (host)

```bash
python3 tests/nr-log-rate/run.py
python3 tests/run_all.py --only nr-log-rate -v
```

The runner extracts and compiles the four real report types and their initializers from
`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp`, then exercises them with the production
`DlssNr_LogRate.h` and injected `Clock::time_point`s. The composition fixture also uses the
production `DlssNr_Exposure.h` white-point law. Acquisition/config containers are host stubs;
the classification and its field wiring are not copied into the test. There are no sleeps.

A second executable checks the generic limiter and recorded trace. Text guards additionally
check that all four sites call it and that GPU timing remains outside it.

## Behaviour covered

- **First observation is always written**, for a struct report and for a scalar. A throttle that
  swallows the first line turns a silent log into evidence of nothing.
- **Identical samples are never written and do not move the window.** Five seconds of them do not
  earn the next drift a delay.
- **The window boundary**: held at one tick under the interval, written exactly at it.
- **Slow drift is not starved.** Ninety-nine steps of 0.01 inside one window are each held, and the
  accumulated difference is written when the window opens, carrying the *latest* value. The same
  trace is also run through a limiter that adopts every sample it observes — the shape this design
  exists to avoid — and that one reports the walk from 1.00 to 2.00 as a single line.
- **A real change bypasses the window** 2 ms after a write, for a setting and for a dimension; and
  it becomes the new window origin, so the drift after it waits a full interval.
- **Recovery after silence**: the first sample after an hour of nothing is written.
- **A clock that does not advance** holds every drift and still passes every real change.
- **Scalar tolerance** is relative to the new value with an absolute floor, checked on both sides of
  both terms.
- `static_assert(Clock::is_steady)`.
- **Real composition classification:** manual white-point scale, source (even with identical
  numerical output), game trim, scan trim/inversion/anchors/collection setting, hold, dimensions
  and detail each change and reverse inside one second. Both events must be immediate. Inactive
  source-specific settings and equivalent clamped trims stay silent; natural drift waits and
  retains the last emitted baseline.
- **Other real sites:** offered exposure availability/auto-exposure flags are immediate;
  pre-exposure alone eventually updates the readback line's derived white point; scan candidate
  changes/reversals, effective scanning state and comparison availability are immediate, while
  measured scan/range drift waits.

## The recorded session

`crimson-compose.trace` is every `DLSS-NR composition` line of one Crimson Desert session
(2026-09-17, build `a9371aab` / `20260917_124410`): 486 lines over 54693 ms, with the call site's quantisation already
applied. It carries the milliseconds, the paper white, and an id for the rest of the line.

Verified against `/home/gabrielmaia/.local/share/Steam/steamapps/common/Crimson Desert/bin64/OptiScaler.log`
(1,774,012 bytes), SHA-256
`708361a76dd3f5f2162c1f1870fccc122fcdb07543efa3fa6f39bf4b357ea52b`.
Line 1 identifies the build; the 486 composition entries span lines 3649–7128,
13:59:47.240638–14:00:41.933328. Truncating each absolute timestamp to milliseconds
before subtracting the first reproduces all fixture timestamps exactly; all values and
signatures also match. The previous 12 September / `393cd0b0` attribution was incorrect.

There are exactly two distinct "rest of the line" values in the whole session, and they differ only
in model size — 1720x720 then 3440x1440. So the session is a real instance of both cases at once: a
white-point drift across 486 reports, and one genuine dimension change that must not be delayed.

Replayed, the suite asserts the old rule would write all 486, the new one writes 41, no held
difference is ever older than one interval, and the model-size change is written on the frame it
happens.

## RED / GREEN

The original source guard failed against the unmodified `DlssNr_Dx12.cpp` at `a9371aab`
(`AssertionError: the limiter is not included`).

For the header, the initial implementation tested four mutations against its generic suite:

| mutation | failures |
|---|---|
| no rate limit at all — the policy this replaces | 159, including `never more than one line per interval`; the replay writes all 486 |
| adopt every observed sample — the starvation defect | 7, including `accumulated drift is written when the window opens` |
| hold real changes too | 6, including `the model size change is written on the frame it happens` |
| swallow the first observation | 12, starting with `first observation written` |

The P2 regression first compiled the actual composition classification and initializer from
the initial throttle implementation: **16 failed assertions**, eight manual changes and their
reversals. Extending the same extraction to the other sites yielded **22 failures** (also missing
pre-exposure-only updates, candidate changes/reversals, scan state and comparison availability).
After adding the configuration identity and completing those report inputs, the real-site cases
and generic/replay suite pass. The baseline had passed both `nr-log-rate` and
`nr-timing-boundary` despite the P2, showing why a stand-in report was insufficient.

Final validation: `python3 tests/run_all.py --only nr-log-rate,nr-timing-boundary
--log-dir /tmp/nr-log-p2-final` passed both suites (2.4 s / 1.5 s). Pinned
clang-format 20.1.8 and `git diff --check` also passed. The final cases additionally
cover effective anchor-table edits below serialization precision and anchor count,
without reading the mutable config string from the render thread.

## What this does not cover

It does not execute NGX, D3D12, a shader or spdlog, or compile the full Windows translation unit.
Only the report types/initializers are extracted from that file and executed. Scan acquisition,
anchor interpolation, game callbacks and concurrency are not exercised. The trace cannot prove
the absence of manual edits because the old log did not record their identity; its 41-line replay
assumes the configuration stayed fixed apart from the recorded dimension change. This counts
emission decisions, not FPS or CPU/GPU savings, and does not validate runtime threading.
