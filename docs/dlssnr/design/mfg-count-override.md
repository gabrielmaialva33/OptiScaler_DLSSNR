# MFG above 2x on Ada under Proton: it works, and what it took to find out

Status: **measured, working, no code change proposed.** The override path was already in the tree and
already correct. Two days went into discovering that nothing was asking it to do anything.

## The result

Cyberpunk 2077, Proton, RTX 4090 (Ada, sm_89), driver 615.71.09, build `1c7e25df`. Frame Generation
on in the game, `Override DLSSG Ratio` set to `4X` from the OptiScaler menu at 13:42.

Presents per rendered frame, in consecutive five-second windows (the DLSS-NR pass runs once per
rendered frame, so its call count is the denominator and `present_id` the numerator):

```
13:41:08 .. 13:42:14    2.00  2.00  2.00  2.00  2.00  2.00  2.00  2.00
13:42:19                3.72                       <- the change
13:42:24 .. 13:44:32    4.00  3.99  4.01  4.00  4.00  4.00  4.01  3.99  ...
```

Exact, and sustained for two minutes. **Nothing in this fork had produced a frame above 2x in a
native-Streamline title before this.**

Note the game's own frame-generation setting still reads 2x while this is happening, and that is
correct: the game asked for 2x and does not know it was overridden. `numFramesToGenerate 1` is what it
requests; `3` is what the plugin receives.

Two things to be careful with when re-measuring. Windows containing a menu or a load produce garbage
ratios (24.64 and 497.62 were observed) because presents continue while the NR pass does not run —
compute per window and discard those, do not average across the session. An earlier pass of this
analysis compared two large blocks and got 3.46 for a period that was cleanly 2.00 throughout.

### Latency

Reported as feeling *lower* at 4x than at 2x, which is the opposite of what generating more frames
should do. Not measured, not explained. Reflex is active and may account for it, or it may be
smoothness being read as latency. Do not repeat the claim without a number; MangoHud can supply one.

## Why it took two days

The override machinery was correct the whole time. Everything that went wrong was diagnosis.

**The mechanism, once seen, is unremarkable.** `hkslDLSSGSetOptions` gates the override on five
conditions at once, and any one of them silently skips it: DLSS-G potentially active, Streamline at
least 2.7.1, `dlssgMfgMax` populated, an override configured, and the count actually differing. The
Ada unlock is a separate mechanism that runs from the LoadLibrary hook and only raises the ceiling —
it patches `nvngx_dlssg.dll` and reports `max generated frames 5 (SL Plugin supports 3, NGX feature
supports 5)`. **A raised ceiling is not a chosen count**, and `OverrideInterpolationCount=auto` chooses
nothing. That was the whole of it.

**Three wrong turns, all the same error.** Absence of a log line was read three times as absence of
execution, and all three times the line was simply below the configured level:

1. F7's `"Neural Rendering key pressed"` is `LOG_DEBUG` and the capture ran at `LogLevel=2`.
2. `hkslDLSSGSetOptions` logs only `LOG_TRACE` (`Streamline_Hooks.cpp:1171`, `:1187`), and
   `SysUtils.h:89` maps that to `spdlog::trace`, level 0, while `LogLevel=1` is level 1.
3. From (2), a whole design note concluded the game bypassed `slDLSSGSetOptions` for the generic
   `slSetData`. It is withdrawn — see `dlssg-options-via-slsetdata.md`, kept for the reasoning error.

**And a hypothesis that survived a day on plausibility alone.** The suspicion was the eOff -> eOn
latch that `98394ab9` works around on the fork's own DLSS-G output. An adversarial review declined to
confirm it without a reproduction, which was correct twice over: not only was it unproven, **it is
false on this path.** The multiplier changes mid-session, with no cycle, and the presents-per-frame
ratio steps immediately. Do not implement a latch workaround here.

## What actually worked as a method

A **one-shot `LOG_INFO` probe**. Ten lines, no log-level change, and it answered in one session each
time — first naming the entry points the game really calls, then reporting the five gates together so
that whichever one was wrong would name itself:

```
DLSSG override state: mode eOn active true sl>=2.7.1 true mfgMax 5 override 3 asked 1
DLSSG override applied: numFramesToGenerate 1 -> 3 (4x)
```

Raising the log level is the reflex and it is the worse tool: it floods, it costs a synchronous flush
per message on the render thread (`LogAsync` defaults false), and as above it can still filter exactly
what is being looked for.

One trap inside the trap, worth writing down because it was walked into immediately. "Log only when it
changes" was implemented as "log when the value being written differs from the value present", which
is true on every call forever — the game asks 1 and the answer is always 3. That produced 5106 lines
in one session. The comparison has to be against *what was last logged*, not against the input.

## Diagnostics left in the tree

Three `LOG_INFO` sites in `hooks/Streamline_Hooks.cpp`, all cheap and all change- or once-triggered.
They cost nothing per frame and they are the reason any of this was answerable; leave them.

- one-shot entry probes in `hkslDLSSGSetOptions` and `hkslDLSSGGetState`, naming viewport and struct
  version (Cyberpunk sends **v3**, which is also what exposed an out-of-bounds read — see `9f49fe70`);
- the five-gate decision line, on change;
- the applied line, on transition.

## Still open

- **Whether 4x is worth using.** Frame pacing, input latency and Ada's software flip metering are
  still unmeasured, and working is not the same as good. What is no longer open is whether it
  *holds*: a later session the same day ran **4.00 presents per rendered frame across 33 consecutive
  five-second windows**, median 4.00, min 3.96, max 4.01, NR coverage 100% throughout. It survived
  four eOff -> eOn cycles and two menu open/close pairs without dropping. An earlier session that
  fell back to 2.00 is unexplained; it also showed windows at 1.00, which looks more like the game
  disabling DLSS-G itself than the override failing, and it is not being chased without a
  reproducible symptom.
- **Whether other titles reach this path at all.** One game has been checked. The gate that matters is
  `dlssgMfgMax` being populated, which needs DLSS-G potentially active at the moment the hook runs.
- ~~`OverrideInterpolationCount` does not persist.~~ Closed the same day: written to the test
  install's ini as `[DLSSG] OverrideInterpolationCount=3`, and a later session came up at 4x from the
  first frame with no menu interaction — the probe's first decision line already reads `override 3`.


## Crimson Desert: `setDynamicMFGParams` under Proton (2026-10-15)

**Finding: a missing Reflex-Sync implementation, not a demonstrated count-override defect.**
No runtime change is made here. The count override is not necessary for the error to continue;
whether the Ada unlock exposes this path cannot be isolated from the retained sessions. Do not
call the error harmless or cost-free: fixed interpolation can continue, but pacing and retry cost
have not been measured. No game was run for this investigation.

### What fails, and why it appears every ten seconds

In `bin64/OptiScaler.log`, the first pair is consecutive (lines 879-880):

```text
[09:50:12.037517] [E] StreamlineHooks::streamlineLogCallback [09-50-12][streamline][error][tid:504][17s:357ms:490us]generic.cpp:1400[setReflexSyncFG] NvAPI_D3D_SetReflexSync((IUnknown*)m_typelessDevice, (NV_SET_REFLEX_SYNC_PARAMS*)&m_cachedReflexSyncParams) failed error -3
[09:50:12.037573] [E] StreamlineHooks::streamlineLogCallback [09-50-12][streamline][error][tid:504][17s:357ms:546us]rsync.cpp:660[setDynamicMFGParams] RSYNC: setDynamicMFGParams failed with status 1
```

The same pair starts the failures in `OptiScaler.previous.log:873-874` and
`OptiScaler.log.pre-ge117:2228-2229`. `-3` is `NVAPI_NO_IMPLEMENTATION`
(`external/nvapi/nvapi_lite_common.h`). The current native-DLSS-G hook forwards this call unchanged
(`OptiScaler/hooks/Reflex_Hooks.cpp`, `hkNvAPI_D3D_SetReflexSync`); its early success branch is for
FSR-FG/XeFG, and the fake-NVAPI implementation also returns success, not -3. The latest log identifies
NVAPI as `C:\windows\system32\nvapi64.dll; native/runtime NVAPI` at line 949. That identifies the
loaded route, not the exact DLL build or a guarantee of Windows driver functionality under Proton.

NVIDIA's published Streamline v2.14.1 implementation of `Generic::setReflexSyncFG` writes the fixed
multiplier, dynamic maximum and target FPS into cached parameters, then calls `NvAPI_D3D_SetReflexSync`.
Its `NVAPI_CHECK` logs the native error once and returns `ComputeStatus::eError`, whose value is 1.
This matches the adjacent log pair and explains why only the outer error keeps appearing. The RSync
caller is in the binary-only DLSS-G plugin, so this is a source-and-log-supported explanation, not
an inspection of that private function's control flow.
Sources: [generic.cpp at v2.14.1](https://github.com/NVIDIA-RTX/Streamline/blob/2122257e0fce486f91b385aa63b9a09b0a34b363/source/platforms/sl.chi/generic.cpp#L1387),
[status and NVAPI_CHECK](https://github.com/NVIDIA-RTX/Streamline/blob/2122257e0fce486f91b385aa63b9a09b0a34b363/source/platforms/sl.chi/compute.h#L533),
[plugin source availability](https://github.com/NVIDIA-RTX/Streamline#readme).

The ten seconds are **our log filter**, not the call interval:
`retoque/hooks/Streamline_LogRepeat.h` sets `Interval = seconds(10)`. For example:

```text
[09:50:39.496147] [E] StreamlineHooks::streamlineLogCallback [09-50-39][streamline][error][tid:504][44s:815ms:695us]rsync.cpp:660[setDynamicMFGParams] RSYNC: setDynamicMFGParams failed with status 1 (repeated 411 more times since the previous line, not written)
```

That represents 412 callbacks in about ten seconds. Startup can write more often while the filter's
256-site table is full and fails open; the old unfiltered session has a median error interval of
29.89 ms. This is a repeatedly failed parameter update on the presentation path, not evidence of
a harmless periodic capability query from the game. `OverrideForceDMFG=false` in both October 15
logs, and fixed `eOn` is what the plugin reports; the method's name does not establish dynamic mode.

### Retained sessions and the useful negative control

Files were read only under `~/.local/share/Steam/steamapps/common/Crimson Desert/bin64/`.
Build stamps date the sessions; rotating names alone do not.

| Log / build | Relevant state | Failure evidence |
|---|---|---|
| `OptiScaler_146683100938.log`, `f034397a`, 2026-09-06 | sl.dlss_g 2.11.1; no configured DLSSG keys or unlock application logged; that build defaults unlock off; plugin reports max 1 and dynamic MFG unavailable | No matching errors in 11,410 lines, 13:58:13–14:06:03; fixed `eOn`, one generated frame, does run |
| `OptiScaler.log.pre-ge117`, `529d8917`, 2026-09-19 | sl.dlss_g 2.14.1; both Ada options true; initially override 3 and forced dynamic mode true; provider advertise/validate/kernel and wrapper DRS patches applied | 15,743 errors, 01:59:18.573754–02:07:10.246591 |
| `OptiScaler.previous.log`, `a2d02037`, 2026-10-15 | sl.dlss_g 2.14.1, provider 310.9.1; both Ada options true, override 3, forced dynamic false | 309 printed lines + 5,656 folded = at least 5,965 callbacks, 00:24:05.775481–00:27:57.051432 |
| `OptiScaler.log`, `acf885e7`, 2026-10-15 | same starting setup; later override changes to 4 then 5 | 278 printed lines + 7,485 folded = at least 7,763 callbacks, 09:50:12.037573–09:54:29.899192 |

Counts include printed lines and their explicit folded counts; unflushed pending repeats can make
the actual total larger. The latest file is a prior `acf885e7` session, not an `a1b86054` capture.

The September 19 log unsets the override mid-session:

```text
[02:05:16.214535] [I] StreamlineHooks::hkslDLSSGSetOptions DLSSG override state: mode eOn active true sl>=2.7.1 true mfgMax 5 override unset asked 1
[02:05:16.219156] [E] StreamlineHooks::streamlineLogCallback [02-05-16][streamline][error][tid:536][378s:174ms:242us]rsync.cpp:660[setDynamicMFGParams] RSYNC: setDynamicMFGParams failed with status 1
```

There are **3,942 further errors after the unset**. At 02:05:46.996414 it again reports `eOn`,
`override unset`, `asked 1`, and errors continue through 02:07:10.246591. Thus the continued failure
does not require an active fixed-count override or dynamic mode. This is not a cold-start control:
previous state and the unlock patches remain. The September 6 session also changes the wrapper
version and available capability, so it cannot prove which change first enabled the failing path.
No retained 2.14.1 session starts with the Ada unlock off.

### Patch and hook audit

- The count assignment comes from the upstream hook (`7a1f18ca`, `a2aef356`); our later
  `1c7e25df`/`150b49d0` edits added diagnostics. A positive override writes
  `numFramesToGenerate`; forcing dynamic mode is a separate option. NVIDIA documents that
  dynamic mode ignores the fixed count and requires a separate support check:
  [DLSS-G guide, sections 6.3 and 14.2](https://github.com/NVIDIA-RTX/Streamline/blob/2122257e0fce486f91b385aa63b9a09b0a34b363/docs/ProgrammingGuideDLSS_G.md).
- `MfgUnlock::TryApply` gates writes on `AdaMfgUnlock`; it patches provider advertise/validate,
  optional kernel selection, the wrapper ceiling and the DRS cap. `MfgPatchPlan.h` commits and
  verifies those sites transactionally; neither component implements or calls Reflex-Sync.
  Raising a capability can nevertheless expose an unsupported pacing path. That indirect
  contribution remains plausible, not established by these logs.
- `6a02e141` added the DRS-cap bypass on September 18, before the first retained failing session.
  `c9d192f8` added the edx ceiling match and transactional writes on October 14, **after** the
  September 19 failures. It cannot be their origin. In 2.14.1 the ceiling is already five and the
  new ceiling site is a no-op. This does not exonerate the older unlock or DRS patch.
- `770e57a4` added the ten-second limiter on October 4. It reduces log writes, not failed calls.
  The earlier bridge investigation in `synthesized-frame-generation.md` also recorded the same
  NVAPI/RSync pair; its observed interpolation is not a pacing-quality measurement for this game.

### Cost, safety and remaining evidence

The latest session logs interpolation enabled with `mode=eOn, numFramesToGenerate=3` immediately
after the first error (line 881). That establishes acceptance of fixed 4x mode, not four distinct,
correctly paced displayed frames. The failed pacing update could affect behavior beyond the log.

There is CPU work on every callback: Streamline's failed call and formatting, our trim allocation
and copy, site string/hash lookup under a mutex, and a counter increment. Retained messages also
format, write and flush synchronously (`LogAsync=false`; `Logger.cpp` uses `flush_on(trace)`).
The limiter avoids most writes after startup, but does not make the retries free. These logs contain
no timing of the failed call or filter; **no milliseconds, FPS loss, or zero-cost claim is justified**.

No safe local fix was demonstrated. Returning success without implementing Reflex-Sync, hiding its
entry point, patching the private RSync retry, or changing the requested mode might silence the log
while changing pacing. None is applied. Upstream `OptiScaler/` files remain byte-identical.
A controlled follow-up needs the same 2.14.1/provider/runtime with a fresh process for unlock off/on
and override unset/3 (forced dynamic off), plus a presentation/pacing trace. Native Windows would
separate a Proton runtime limitation from a capability exposed by the unlock. These are the missing
controls, not tests performed by this change.
