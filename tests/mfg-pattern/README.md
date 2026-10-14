# MFG byte-pattern matching

Run `python tests/mfg-pattern/run.py` from the repository root. The runner builds in an
automatically removed temporary directory with g++ C++20, ASan and UBSan, and `-Werror`
(with `-Wno-conversion-null` only, because the sliced production `FindPattern` returns
`NULL` as a `uintptr_t` on a miss and that idiom is not this test's to change). It fails on
compiler errors, sanitizer findings or a failed assertion.

## What it exercises

The MFG unlock (`retoque/framegen/dlssg/MfgUnlock.cpp`) patches `nvngx_dlssg.dll` and
`sl.dlss_g.dll` in memory by finding a byte signature and rewriting bytes at a fixed offset
inside it. Whether a patch lands at all is decided by the signature match, so that is what
this suite covers.

It does **not** copy the signatures or the matcher. `run.py` slices the real
`FindPattern` out of `OptiScaler/scanner/scanner.cpp` (a free function that walks a raw
range, so unlike `scanner::GetAddress` it needs no PE image and compiles on the host) and
the ten real `kXxxPattern` string constants out of `MfgUnlock.cpp`, then injects both into
`test.cpp` at a marker. The strings under test are byte-for-byte the ones the shipped DLL
patches with; if a signature is retuned in production, this suite tests the new one.

`test.cpp` builds synthetic byte buffers and asserts:

- each architecture signature (`kValidatePattern`, `kValidatePattern309`, `kAdvertisePattern`,
  `kAdvertisePattern309`) matches a correct encoding of its site, where the compare against
  the Blackwell arch id `0x1b0` is a `cmp` — `3D` for `cmp eax, imm32`, `81 /7` for
  `cmp r/m32, imm32`;
- the same buffer with the `cmp` opcode swapped for a `mov` of the identical immediate
  (`B8`–`BF`, or `C7 /0`) does **not** match, so a matcher keyed on the immediate alone —
  which would patch a `mov` and corrupt the DLL — is ruled out;
- a bare `cmp eax, 0x1b0` with none of the surrounding branch/count shape does not match, so
  the signature is the site's shape and not just the constant;
- `kWrapperClampPattern` matches the `sl.dlss_g.dll` `min(published, 3)` clamp, which
  legitimately keys on a `mov` immediate (the wrapper's own ceiling of 3), confirming the
  discriminator is the `cmp` against the arch id and not "never match a `mov`";
- the wrapper clamp is found in both register allocations: `kWrapperClampPattern` (`r8d`,
  Streamline 2.7.1) with its ceiling at three or already at five, and `kWrapperClampPatternEdx`
  (`edx`, every other wrapper seen: 2.7.3 to 2.14.1) at three or five; the `r8d` pattern does
  not find the `edx` shape, which is the miss the second pattern exists for, and an `edx` clamp
  whose `cmovb` source another unlocker rewrote is not matched;
- a gate whose architecture constant was rewritten to `0x190` (what MFGAdaUnlock writes) is missed
  by the exact pattern and found by its `k...ForeignPattern` counterpart, in both the 310.9 and the
  older shapes, so `MfgUnlock` reports it as another unlocker's work and leaves it alone;
- a buffer holding every `mov`-immediate encoding of `0x1b0` end to end is matched by none of
  the four architecture signatures nor their four foreign counterparts.

The shapes are not invented. Each was counted in the executable sections of real
`nvngx_dlssg.dll` (310.2.1, 310.7.129, 310.9.1) and `sl.dlss_g.dll` (2.7.1, 2.7.3, 2.7.30,
2.8.0, 2.9.0, 2.9.1, 2.14.1) copies from installed games on 2026-10-14, and each matched exactly
once per file; `scanner::GetAddress` returns the first match, so uniqueness is a property of the
builds, not something the matcher enforces.

## What it does not cover

It runs no patcher and touches no DLL. `PlanAdvertise`, `PlanValidate`,
`PatchBlackwellKernels`, `PlanWrapperClamp` and `PlanSlDrsClamp` also call `VirtualProtect`,
rewrite bytes and (for the fatbin path) walk PE sections; none of that runs here, and neither
does `TryApply` or its summary log. The commit and rollback those sites go through are
`tests/mfg-patch-plan`'s. A green run says the signatures still discriminate `cmp`
from `mov` against a synthetic buffer — nothing about the real modules, whose exact bytes
shift between driver versions, or about whether an unlocked count actually produces frames.
In-game validation on real `nvngx_dlssg.dll` / `sl.dlss_g.dll` remains the only proof of that.
