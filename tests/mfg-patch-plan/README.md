# MFG patch commit and rollback

Run `python tests/mfg-patch-plan/run.py` from the repository root. It builds `test.cpp` in an
automatically removed temporary directory with g++ C++20, ASan, UBSan and `-Werror`, and fails
on a compiler error, a sanitizer finding or a failed assertion.

## What it exercises

The MFG unlock (`retoque/framegen/dlssg/MfgUnlock.cpp`) finds every site a patch needs first and
writes them afterwards, as one group, through `MfgPatch::Commit` in
`retoque/framegen/dlssg/MfgPatchPlan.h`. That header has no Windows dependency, so this suite
includes it unchanged and drives it over a byte buffer whose writes can be told to fail.

It asserts that:

- planning reads and records but writes nothing;
- a group of good writes commits, every site holds its replacement and nothing around it moves;
- a site that already holds its replacement (a wrapper ceiling already at five) is never written,
  and a group of only such sites reports `AlreadyApplied`;
- a write that fails part way, at any position in the group, restores every touched site,
  including the half-written one;
- a write that reports success but lands the wrong bytes is caught by the read-back and rolled
  back;
- a site that changed between planning and committing makes the whole group `Stale` with no
  write at all;
- a restore that fails is reported as `RollbackFailed`, and the restores after it still run;
- a site of zero bytes, or longer than `kMaxSiteBytes`, is refused at planning time.

## What it does not cover

`VirtualProtect`, `FlushInstructionCache` and the real modules. The production `LiveOps` in
`MfgUnlock.cpp` is a `memcpy` read and the existing `WriteBytes`; whether a real write can fail
part way is not something this buffer can tell. The signature search that produces the sites is
`tests/mfg-pattern`'s. Whether an unlocked count produces frames needs a game.
