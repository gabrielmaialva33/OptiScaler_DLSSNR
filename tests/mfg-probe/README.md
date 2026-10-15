# MFG-below-Ada probe (step 0)

Run `python tests/mfg-probe/run.py` from the repository root. The runner builds `test.cpp` in an
automatically removed temporary directory with g++ C++20, ASan, UBSan and `-Werror`, and fails on a
compiler error, a sanitizer finding or a failed assertion.

## What it exercises

The step-0 probe of [`docs/dlssnr/design/mfg-rtx20-30.md`](../../docs/dlssnr/design/mfg-rtx20-30.md) looks
at the DLSS-G provider a 20/30-series machine loads and logs what it finds. It **writes nothing**: no
memory patch, no file, no ini or Config key. All of its deciding, counting and formatting lives in
`retoque/framegen/dlssg/MfgProbe.h`, which is std-only; this suite compiles that header straight from
production (not a copy) and drives it.

It asserts:

- **The gate is first.** `RunModuleProbe` and `RunAdmissionProbe` take an injected environment. Off
  `[Turing, Ada)` — Ada (`0x190`), Blackwell (`0x1b0`), an id above it, Volta (`0x150`), an NVIDIA adapter
  whose architecture was never read (`0`, what `IdentifyGpu` reports when its NvAPI query cannot run), and a
  non-NVIDIA vendor with arch `0` or an Ampere-like id — the environment's claim, foreign-loader check,
  census, fact-gathering and emit are **never called**, counted through the fake environment. Everything
  stateful the production glue does (resolving and de-duplicating the module, the admission's once-only
  claim, bringing the NGX core up) is one of those calls, so the count covers it. On Ampere and Turing the
  whole sequence runs exactly once; a module already described stops at the claim; the admission line is
  gathered once however often feature 11 is asked about. The boundaries are checked: `TU100` and one id
  below `AD100` included, `AD100`, one id below `TU100` and `0` excluded.
- **The production gate memo** (`GateCache`, what `LogModule`/`LogAdmission` consult first): on Ada the GPU
  is read once and every later call is answered from the memo; an NVIDIA adapter with an unread
  architecture is decided inert and memoised; an unidentified GPU is answered "no" and not memoised, and
  once identified as Ampere the "yes" is memoised; a non-NVIDIA adapter is inert.
- **CUDA loadability on sm_86** (`LoadableOnSm86`): PTX is forward-only (sm_86 and sm_75 PTX load, sm_89
  and sm_120 do not); SASS runs on the same major at an equal-or-lower minor (sm_86 and sm_80 load, sm_89,
  sm_75 and sm_120 do not).
- **The census** over synthetic fatbin containers and bare CUDA ELF cubins: a frame container (one with a
  Blackwell/sm_120 PTX) with an sm_86 SASS is loadable; a Blackwell+Ada frame container is not; a PTX-only
  sm_89 container is a network container, not a frame one; bare cubins are counted by SM; an ELF image
  sitting **inside** a container is not mistaken for a bare cubin; a mixed module tallies all three
  together; a container whose declared payload runs past the buffer is skipped rather than read past
  (ASan would catch an overrun); and a container length of `2^64-1`, `2^64-16` or `2^64-17`, or an image
  payload of `2^64-1` or header size of `2^32-1`, ends the walk instead of wrapping the bound (`2^64-16`
  used to put the cursor back on the same container forever; `run.py` gives the binary a 120 s timeout so a
  regression fails fast).
- **The exact text** of the module line (matched byte-for-byte against the example in the note's "Step 0
  as built" section) and the admission line (a stock below-Ada refusal, `0xbad00001` / flags `0x4`, and
  each reason the query can be skipped for: the core not loaded, DLSS off or the adapter not DLSS-capable,
  a core without the entry), plus the spoofed-HAGS and foreign/missing-gate renderings.
- **Foreign-loader detection**: the lines for a loaded `sm86_backend.dll`, a `dlssg_sm86.ini` beside the
  game, and both at once.

## The real provider, when present

If `~/.local/share/Steam/steamapps/common/Crimson Desert/bin64/nvngx_dlssg.dll` is installed, the runner
passes its path to the binary, which parses the PE off disk, feeds each non-executable section to the
census exactly as the production glue feeds the mapped sections, and holds the result against the offline
inventory: no frame kernel loadable on sm_86, 39 network cubins for sm_86 and 39 for sm_89, 31–38
frame-construction containers, and every non-frame container one of the 39 network kernels (so 70 in the
310.9.1 build). The file is read in place and **never copied**; `run.py` builds the path from
`Path.home()` so no home path is written into a tracked file (`tests/repo-privacy` guards that). The case
**skips** when the file is absent.

## What it does not cover

It runs no patcher, loads no DLL, issues no NGX or Streamline call, and never compiles the Windows glue in
`MfgProbe.cpp`: the concrete environments (the PE reads, the sha256 through a lazily resolved `BCryptHash`,
the driver-version and HAGS reads, `MfgUnlock::ProbeGates`, the NGX bring-up and the one capability query)
and the two thin entry points that consult `GateCache` and hand an environment to the drivers. Those run
only on a below-Ada card in a game, which is the whole point of step 0: see the note's "Test plan with
Rafael's RTX 3060". A green run says the gate, the decision logic, the census and the line text are right —
nothing about what NVIDIA's runtime actually answers on hardware it declines.
