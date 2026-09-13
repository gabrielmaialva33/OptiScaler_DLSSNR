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

- **Whether 4x is worth using.** It works; frame pacing, input latency and the software flip metering
  on Ada are unmeasured. Working is not the same as good.
- **Whether other titles reach this path at all.** One game has been checked. The gate that matters is
  `dlssgMfgMax` being populated, which needs DLSS-G potentially active at the moment the hook runs.
- **`OverrideInterpolationCount` does not persist.** It was set from the menu; the ini still says
  `auto`. A count that has to be re-picked every launch is a setting only its author will use.
