# Reaching DLSS-G options in games that call slSetData instead of slDLSSGSetOptions

Status: **premise withdrawn, design not implemented.** Read the correction at the bottom before
anything else: the evidence this note was built on turned out to be a filtered log level, so the
claim that Cyberpunk 2077 bypasses `slDLSSGSetOptions` is *not established*. The body is kept as
written, because the mechanism research in it is still correct and the reasoning error is worth
being able to see.

## What was assumed, and what is actually true

The open question since 2026-09-12 was whether the native Streamline path suffers the same latch as
`98394ab9`: the sl.dlss_g plugin only latches `numFramesToGenerate` on an eOff -> eOn transition, and
`StreamlineHooks::hkslDLSSGSetOptions` (`hooks/Streamline_Hooks.cpp:1196-1210`) writes the count and
calls straight through with no such cycle. An internal review declined to call it confirmed without a
reproduction, which was right.

The reproduction says something else. In a full Cyberpunk 2077 session with debug logging:

```
13101x  StreamlineHooks::hkslEvaluateFeature
   13x  StreamlineHooks::hkdlss_slGetPluginFunction
    0x  StreamlineHooks::hkslDLSSGSetOptions
```

and from the plugin's own log, the entry points the game actually drives:

```
8x  dlss_gEntry.cpp:2030[slSetData]
1x  dlss_gEntry.cpp:1862[slGetData]
```

**Cyberpunk 2077 configures DLSS-G through `slSetData`, the generic data API, not through
`slDLSSGSetOptions`.** The latter is a convenience wrapper that chains a `ViewportHandle` and a
`DLSSGOptions` and calls `slSetData` itself. This fork hooks only the wrapper.

The latch question is therefore not answerable here and was mis-posed: the override is not being
latched or ignored, it is **never sent**.

### The consequences, each verified

1. `hkslDLSSGSetOptions` never runs.
2. So `State::dlssgMfgMax` is never populated — it is only assigned inside that hook
   (`Streamline_Hooks.cpp:1186`).
3. So the "Override DLSSG Ratio" combo never appears: the menu gates it on
   `state.dlssgMfgMax.has_value()` (`menu/menu_common.cpp:3483`).
4. So `FGDLSSGOverrideInterpolationCount` is unreachable in this title, whatever the ini or the menu
   says.

The Ada MFG unlock is unaffected and does work: it patches `nvngx_dlssg.dll` from the LoadLibrary hook
(`hooks/LibraryLoad_Hooks.cpp:122`), independently of who drives the options, and the session log
confirms it — `31 kernel containers answer Ada with the Blackwell image`, `sl.dlss_g.dll ceiling
raised to 5`, `Multi-frame supported, max generated frames 5`. The ceiling is raised. There is simply
no path by which a count above the game's own choice can be requested.

## The mechanism to hook

`slSetData(const sl::BaseStructure* inputs, sl::CommandBuffer* cmdBuffer)` — the typedef already
exists as `PFN_slSetData` in `hooks/Streamline_Hooks.h:135`; nothing is attached to it.

`inputs` is the head of a chain. `BaseStructure` carries `next`, `structType` and `structVersion`
(`external/streamline/sl_struct.h:106`), and the SDK ships the walk as `findStruct<T>`
(`sl_helpers.h:416`), which follows `next` until `structType == T::s_structType`. The two structs that
matter are identified by GUID:

| struct | GUID | where |
|---|---|---|
| `DLSSGOptions` | `FAC5F1CB-2DFD-4F36-A1E6-3A9E865256C5` | `sl_dlss_g.h:72`, `kStructVersion5` |
| `ViewportHandle` | `171B6435-9B3C-4FC8-9994-FBE52569AAA4` | `sl_core_types.h:589`, `kStructVersion1` |

`DLSSGOptions::numFramesToGenerate` is the field, and the header states the mapping plainly: 1 is 2x,
2 is 3x, 3 is 4x.

So one hook on `slSetData` covers both call styles, because the wrapper path ends up here too. That is
the argument for doing it at this level rather than adding a second special case.

## Design

### Where it goes

A new `hkslSetData` in `hooks/Streamline_Hooks.cpp`, attached alongside the existing interposer hooks
in `hookInterposer`, and detached in `unhookInterposer` next to the others.

### What it does

1. `findStruct<DLSSGOptions>(inputs)`. If absent, tail-call the original untouched. This is a hot,
   shared entry point — every Streamline data write in the process passes through it — and everything
   that is not a DLSS-G options write must cost one GUID comparison per chain link and nothing else.
2. If present, `findStruct<ViewportHandle>(inputs)` for the viewport. Absent viewport means we cannot
   attribute the options; pass through untouched rather than guess a viewport.
3. Apply the same policy `hkslDLSSGSetOptions` already applies, on a **copy**, then send the copy.

### The copy is not optional

`inputs` is `const` and belongs to the caller. The existing wrapper hook receives `const
sl::DLSSGOptions&` and builds its own `newOptions`; this one must do the same and additionally rebuild
the chain, because the chain links are `next` pointers into caller memory. Concretely: copy the
`DLSSGOptions` by value, point the copy's `next` at whatever the original pointed to, and re-link the
predecessor — or, simpler and safer, copy only the options struct and pass a chain head that is our
copy with the original's `next` preserved, leaving every other struct in the chain untouched and
still owned by the caller.

Mutating the caller's struct in place would be the short path and must not be taken: the caller may
hold that struct across frames, and a game that reads back its own options would see ours.

### What policy to apply

Exactly what the wrapper hook does today, factored out so there is one implementation and not two:

- the `FGDLSSGOverrideInterpolationCount` override, including the clamp against `dlssgMfgMax` and the
  `overrideCount == 0` means eOff case (`Streamline_Hooks.cpp:1197-1205`);
- the one-time `dlssgMfgMax` population, which is what makes the menu control appear at all
  (`1179-1194`);
- `applyMenuDlssgInterlock`, which turns FG off while a Vulkan overlay is up (`1756`);
- `state.dlssgLastSetMode` bookkeeping (`1207`).

Factoring is the point of the change as much as the new hook is. Two copies of this policy would
diverge, and the second copy is the one nobody tests.

### Ordering against the existing hook

Both can be installed. `slDLSSGSetOptions` calls `slSetData` internally, so in a game that uses the
wrapper the policy would be applied twice — once in each hook. That is not harmless: the override is
idempotent, but `dlssgMfgMax` population issues a `slDLSSGGetState` and the interlock has side
effects. The straightforward resolution is a per-call re-entry guard (thread-local depth counter set
by the wrapper hook, checked by the data hook), which is also the honest one: it says out loud that
the wrapper is a caller of the thing we now also hook.

## What this does not settle

**The latch is still unknown.** Once options actually reach the plugin, changing the count mid-session
may or may not require the eOff -> eOn cycle that `98394ab9` implements for the fork's own DLSS-G
output. That is a second experiment, and it can only be run after this one lands, because today the
count never arrives at all. Do not implement a latch workaround speculatively on this path.

**Whether MFG above 2x is usable on Ada under Proton is also unknown.** The unlock raises the ceiling
and the kernel retargeting is in place, but nothing in this fork has ever produced a frame at 3x or
4x in a native-Streamline title. Frame pacing, latency and the software flip metering on Ada are all
unmeasured. The first result from this change should be treated as a measurement, not a feature.

**No other title has been checked.** `slSetData` is what Cyberpunk uses; whether the fourteen other
installed games use the wrapper, the data API, or neither has not been surveyed. The hook is worth
having either way, but the claim "this fixes MFG override generally" is not established by one log.

## How the current state was established

Cyberpunk 2077, Proton, RTX 4090, driver 615.71.09, build `dd5c1e44`, `[Log] LogLevel=1`. Counting
hook invocations by name in the log is enough:

```sh
grep -oE "StreamlineHooks::hk[A-Za-z_0-9]+" OptiScaler.log | sort | uniq -c | sort -rn
grep -oE "dlss_gEntry\.cpp:[0-9]+\[[a-zA-Z]+\]" OptiScaler.log | sort | uniq -c | sort -rn
```

The second line reads the plugin's own logging, which this fork forwards through
`StreamlineHooks::streamlineLogCallback`, and is what named `slSetData` as the entry point in use.

---

## Correction (2026-09-13, same day): the diagnosis above is not established

An adversarial review of this note found the central evidence unsound, and the finding is verified.

**`hkslDLSSGSetOptions` logs only at TRACE** — `Streamline_Hooks.cpp:1171` and `:1187`, both
`LOG_TRACE`, which `SysUtils.h:89` maps to `spdlog::trace`, level 0. The capture that this note was
built on ran at `LogLevel=1`, which is `debug`, level 1. **The hook could have run on every frame and
written nothing.** Its absence from the log is not evidence that it did not run, and every conclusion
drawn from that absence is withdrawn.

This is the same mistake twice in one session, one level apart: first reading a filtered `LOG_DEBUG`
at `LogLevel=2` as proof that F7 never registered, then reading a filtered `LOG_TRACE` at
`LogLevel=1` as proof that the wrapper is never called.

Three further errors in the note, all confirmed against the source:

- **The causal chain is wrong.** `dlssgMfgMax` is not populated only by the setter;
  `hkslDLSSGGetState` assigns it too (`Streamline_Hooks.cpp:1269`). "The menu control never appeared"
  therefore does not identify which hook failed to run.
- **The override does not depend on that query succeeding.** The setter applies
  `FGDLSSGOverrideInterpolationCount` regardless of the capability lookup's result
  (`Streamline_Hooks.cpp:1198`), so a missing maximum does not by itself block an ini override.
- **The 13 `hkdlss_slGetPluginFunction` calls are DLSS SR, not DLSS-G.** The DLSS-G factory hook is
  `hkdlssg_slGetPluginFunction` and its `LOG_DEBUG` is commented out (`Streamline_Hooks.cpp:1390`),
  so that count says nothing about DLSS-G either.

And one architectural premise is wrong independently of the evidence problem: **there is no global
`slSetData` in the interposer.** It is a per-plugin function handed out by each plugin's
`slGetPluginFunction`; DLSS SR has its own. So "one hook on slSetData covers every Streamline data
write" is false, and with it the main argument for hooking at that level rather than at the DLSS-G
plugin. If a hook is ever warranted here, it should target the active DLSS-G plugin, with the
supported struct types and versions named explicitly.

The chain design was also broken as written: making the options copy the chain head with
`copy.next = original.next` **drops the ViewportHandle**, which precedes the options in the chain the
wrapper builds. Re-linking the caller's predecessor would write into caller memory, and cloning
unknown nodes is impossible because `BaseStructure` carries no size. A working design must enumerate
which prefixes and versions it supports and pass the original through untouched for everything else.

### What replaces it

`3e8ae685`'s premise is unproven, so nothing is implemented. Instead, a one-shot `LOG_INFO` was added
at the top of both `hkslDLSSGSetOptions` and `hkslDLSSGGetState` reporting entry, viewport and struct
version. One ordinary `LogLevel=2` session then answers, without a trace-level capture and without a
design resting on an inference:

- both lines present -> the wrapper is reached, this note's premise is false, and the MFG override
  question is about policy or the latch, not about the entry point;
- neither -> the wrapper genuinely is not reached, and the plugin-level design becomes worth writing
  properly;
- getter only -> the interesting case, and the one the original evidence could not have distinguished.

Do not implement any of the design above until one of those three is observed.

### Observed (2026-09-13): both lines present, and the whole question resolved

The probe fired on both entry points — `the game calls slDLSSGGetState (viewport 0, struct v3)` and
`the game calls slDLSSGSetOptions (viewport 0, struct v3, mode eOff, numFramesToGenerate 1)`. The
wrapper is reached. This note's premise is false and nothing here will be implemented.

What the override actually needed was for something to ask for a count. See
[mfg-count-override.md](mfg-count-override.md): MFG at 4x is now measured working on Ada under
Proton, there is no latch on this path, and the struct v3 the probe reported also exposed an
out-of-bounds read fixed in `9f49fe70`.
