# NR pass configuration verification

Run from the repository root:

```sh
python tests/nr-pass-config/run.py
```

Requires Python and GCC with C++20/ASan/UBSan. Temporary generated sources and binaries
are deleted automatically. This directory is outside the release package allow-list.

The suite compiles the production portable codec and extracts the actual Config snapshot
and mutation methods, using the real CustomOptional implementation and SimpleIni library.
It covers strict numeric parsing, finite/ranged tuning, bounded counts, all seven sparse
keys, first-valid alias precedence, live master inheritance, explicit master-equal values,
canonical serialization, real file round trips, removal, retained inactive passes, no
insertion during reads, and 5,000 concurrent snapshot/mutation transactions. ASan/UBSan
are not a race detector. Passing does not prove NGX behavior, GPU resource lifetime or
menu integration. The full Windows Config translation unit is checked by Release build.

## Integration API and locking

`GetDlssNrPassSnapshot()` returns Count, Individual and all three resolved Settings under
one short lock. Renderer code should read it once per recording. Pass indices in APIs are
zero-based; INI section numbers are one-based. `GetDlssNrMasterSettings()` returns the pure
master, and `GetDlssNrPassOverrides()` returns a copy of sparse values without inserting.
`SetDlssNrPassOverrides()` validates and replaces a pass; an empty value erases it.
`ClearDlssNrPassOverrides()` removes the entire pass. Empty fields inherit live masters.

Menu writes must use `SetDlssNrPassCount`, `SetDlssNrIndividualPassSettings` and
`SetDlssNrMasterSetting(&Config::DlssNrIntensity, value)` (or the corresponding model
member), with local UI values instead of writing through CustomOptional pointers. Never
hold a config lock across a renderer retry: the renderer may acquire config while owning
its own lock. These APIs do not make unrelated legacy CustomOptional fields thread-safe.

Reload reads the sparse namespace into a fresh SimpleIni instance because the legacy
shared SimpleIni object merges LoadFile calls. This avoids resurrecting sections removed
from disk. Master keys retain the repository's existing first-config-wins semantics.
Saving owns only valid `DlssNr.PassN` sections: unknown keys in those sections are removed;
other sections, including malformed names, are not changed. Canonical section numbers are
1 through UINT32_MAX; runtime pass count is independently limited to three.

## Adversarial config review

Both Passes (default 1) and IndividualPassSettings (default false) have declaration, read,
save and shipped-INI entries. Default master values and disabled inheritance are unchanged.
Strict parsers are scoped to NR model keys; general parsers/float serialization are unchanged.
Aliases are merged first-valid per key and rebuilt canonically on save; stale field and
pass removal cannot reappear from the persistent INI object. Explicit values are never
collapsed merely because they currently equal their master. All mutable override storage
is private and every exposed snapshot/mutation is locked. No shader constants, shaders,
resource lifetimes, renderer entry points or game files are changed by this component.
