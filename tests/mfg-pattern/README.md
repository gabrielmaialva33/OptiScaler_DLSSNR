# MFG byte-pattern matching

Run `python tests/mfg-pattern/run.py` from the repository root. The runner builds in an
automatically removed temporary directory with g++ C++20, ASan and UBSan, and `-Werror`
(with `-Wno-conversion-null` only, because the sliced production `FindPattern` returns
`NULL` as a `uintptr_t` on a miss and that idiom is not this test's to change). It fails on
compiler errors, sanitizer findings or a failed assertion.

## What it exercises

The MFG unlock (`OptiScaler/framegen/dlssg/MfgUnlock.cpp`) patches `nvngx_dlssg.dll` and
`sl.dlss_g.dll` in memory by finding a byte signature and rewriting bytes at a fixed offset
inside it. Whether a patch lands at all is decided by the signature match, so that is what
this suite covers.

It does **not** copy the signatures or the matcher. `run.py` slices the real
`FindPattern` out of `OptiScaler/scanner/scanner.cpp` (a free function that walks a raw
range, so unlike `scanner::GetAddress` it needs no PE image and compiles on the host) and
the five real `kXxxPattern` string constants out of `MfgUnlock.cpp`, then injects both into
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
- a buffer holding every `mov`-immediate encoding of `0x1b0` end to end is matched by none of
  the four architecture signatures.

## What it does not cover

It runs no patcher and touches no DLL. `PatchAdvertise`, `PatchValidate`,
`PatchBlackwellKernels`, `PatchWrapperClamp` and `PatchSlDrsClamp` also call `VirtualProtect`,
rewrite bytes and (for the fatbin path) walk PE sections; none of that runs here, and neither
does `TryApply` or its summary log. A green run says the signatures still discriminate `cmp`
from `mov` against a synthetic buffer — nothing about the real modules, whose exact bytes
shift between driver versions, or about whether an unlocked count actually produces frames.
In-game validation on real `nvngx_dlssg.dll` / `sl.dlss_g.dll` remains the only proof of that.
