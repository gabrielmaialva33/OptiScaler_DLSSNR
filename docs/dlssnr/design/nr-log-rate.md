# Rate-limiting the NR drift logs

## The measurement

Crimson Desert log of 2026-09-17, build `a9371aab` / `20260917_124410`,
54.7 s of composition, `LogLevel=2`:

| line | count | rate |
|---|---|---|
| `DLSS-NR composition` | 486 | 8.9/s, pairs as close as 22 ms |
| `DLSS-NR exposure scan` | 228 | 4.2/s |
| `DLSS-NR game exposure ... -> white point` | 137 | 2.5/s |
| `DLSS-NR exposure from the game` | 1 | — |

The 486 composition lines have two distinct non-white-point signatures: 82 at
1720x720 and 404 at 3440x1440. There is one model-size transition; the remaining
changes are in white point. That dimension event must bypass the limiter. The old
log lacks configuration identity, so the replay treats those changes as drift; it
cannot establish whether any came from a manual control edit.

Provenance verified against `Crimson Desert/bin64/OptiScaler.log`: 1,774,012 bytes,
SHA-256 `708361a76dd3f5f2162c1f1870fccc122fcdb07543efa3fa6f39bf4b357ea52b`.
Build is on line 1; composition runs from line 3649 (13:59:47.240638) to line 7128
(14:00:41.933328). The fixture matches all 486 values/signatures/times exactly
when each timestamp is truncated to milliseconds before subtracting the first.
Earlier fixture/README attribution to 2026-09-12 / `393cd0b0` was incorrect.

The existing quantisation is a precision limit, not a rate limit, and a
measured white point crossed the existing 0.01 boundary about nine times a second
in this trace. The remaining cost is per-line: `LogAsync=false` and spdlog's `flush_on(trace)` mean each
line is a formatted write plus a flush on the render thread.

## What changes

Nothing about *what* is reported, only *when* a repeat of a drifting quantity is allowed.

- **Drift** — a measured number: white point, the game's exposure, the scan's candidate. Rate
  limited to one line per second per site.
- **Shape** — everything else in the same report: configuration (`TransferStrength`,
  `ColourStrength`, `MaxRatio`, `Transfer`, `DebugView`, `Compare`), the discrete passthrough
  decision, and the model dimensions. A shape change is written immediately; it is an event, not a
  trend. White-point configuration is also shape: source, validated slider fallback,
  the applicable game-exposure trim, scan inversion/trim/effective anchor table/collection
  setting when source 2 is selected, and hold state. A computed white point is not
  sufficient identity: manual edits and reversals must be reported even inside one
  second, or when they happen to produce the same rounded number. Read the anchor table
  through the existing mutex-protected Anchors() snapshot: copying the unprotected
  configuration string on the render thread would race with menu serialization writes,
  and serialization rounds values to six significant digits. Inactive source-specific
  controls are normalized out. These are report-only snapshots; no render parameters change.
- **First observation** of any report is always written. A throttle that swallows the first line
  turns a silent log into evidence of nothing.

GPU timing samples, errors, and everything the visual path does are untouched. This decides
whether to call `LOG_INFO`; it does not decide anything the shader sees.

Other sites: offered exposure keeps availability/auto-exposure flags as immediate
events. The game-exposure value line has no user-configured transform; both its
exposure and pre-exposure are measured drift. The scan diagnostic has no anchor or
trim transform either, but candidate identity, effective scanning state (source 2
or diagnostic collection enabled), and availability
of the game's comparison value are immediate events. Its numeric values remain
rate-limited. No new synchronization or changes to acquisition are introduced.

Regression before the P2 fix: compiling the actual ComposeReport and its initializer
fails 16 assertions (eight manual changes plus their reversals). Tests use injected
time and the production white-point fallback/game-exposure law; the original generic
Report-only suite missed these dependencies. Extending the extraction to all four
sites produced 22 failures, including pre-exposure-only changes, candidate changes,
scan state and comparison availability. All four report types and initializers now
compile and pass, alongside the existing generic limiter/replay coverage. The host
fixture stubs acquisition/config containers; it does not run the renderer or scan
interpolation. Baseline passed nr-log-rate and nr-timing-boundary before the new cases.
Final `tests/run_all.py --only nr-log-rate,nr-timing-boundary` passes both suites;
nr-log-rate also covers anchor edits below serialization precision, anchor count,
inactive settings and equivalent clamped trims. Only host validation was performed.

## The trap this is built around

The obvious implementation updates the remembered report on every sample and logs when the sample
differs from the previous one. Under a rate limit that starves: each suppressed step is small, the
baseline creeps along with the value, and a quantity that walks from 1.38 to 3.00 over a minute is
never reported at all, because no single comparison ever saw it move.

So the remembered report is the **last line actually written**, never the last sample observed. A
suppressed sample changes nothing. When the window opens, the comparison is against what the log
says, so accumulated drift is still a difference and is still written.

`Reporter<Report>` in `DlssNr_LogRate.h` makes that structural rather than a convention: the
baseline is private and the only function that writes it is the one that returns "emit". A caller
cannot adopt a sample it did not log.

## Clock

`std::chrono::steady_clock`. A wall clock can step backwards over NTP or a suspend and silence a
site for as long as the correction. The tests inject the time point, so they have no sleeps.

## Limits

- The interval is a constant (`DriftInterval`, 1 s), not an INI key. A key would owe the four-point
  round trip of `DEVELOPMENT.md` invariant 4 and a menu row it does not need; `LogLevel` already
  turns these lines off.
- Per-site state, `static` at the call site, no lock — the same threading assumption the counters
  it replaces already made.
- This is a log-volume change. It says nothing about frame time, and nothing here was measured on
  a GPU.
- Composition owns a snapshot of the effective anchor table while source 2 is active;
  copying it can allocate and takes the existing scan mutex once more. It avoids a new
  unsynchronized read of the mutable configuration string, but the CPU cost has not
  been measured. Other scalar configuration access/threading assumptions are unchanged.
