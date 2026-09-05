# Optional localization verification

Run from the repository root:

```sh
python3 tests/nr-localization/run.py
```

Requires g++ with C++20 and ASan/UBSan. Temporary outputs are outside the repository; tests
are not part of the shipped DLL or release package. No game directory, marker, locale or
font configuration is changed.

The suite compiles the real portable dictionary parser and the repository's complete
`imgui.cpp`, linking its real `ImHashStr`. It extracts actual literal label call sites from
both modified menu files and requires at least 60 to prevent silent zero coverage. It checks
original IDs across four seeds, bare/`##` fallback, preserved `###` IDs, identical translated
words with distinct identifiers, original English pointers, UTF-8, malformed files,
transactional rejection, printf argument order, width/precision stars, lengths, percent
literals and the shipped Portuguese pack.

A separate executable compiles production `Localization.cpp` with only DLL-path/logging/PCH
platform stubs. Four separate processes check absent, inactive-subdirectory, invalid and
explicitly active dictionaries. They also check that edits after loading cannot invalidate
returned pointers or silently change language. Files exist only in temporary test directories.

Passing means every assertion succeeds and both executables exit 0 with sanitizer errors
absent. It does not prove Windows DLL integration, menu rendering, text layout or font glyphs;
Release compile/link and a later authorized UI check are separate validations.

See [pack documentation](../../OptiScaler/translations/README.md) for activation and the
explicitly partial coverage. No `###` suffix is invented for a previously bare control.

## Adversarial review

- No config settings or NR values changed; no visibility condition or slider range changed.
  The white-point source/anchor table in DEVELOPMENT.md remains structurally identical.
- Text uses `Tr`; widgets use `Label`. Hidden IDs never leak into help/status text.
- Dictionaries cannot inject format arguments or hidden IDs. Unknown or rejected text falls
  back to the exact original string; inactive packaging does not select a language.
- Bare and `##` labels remain English because the real ImGui hash test disproved the tempting
  `translation###original` shortcut. Existing `###` suffixes survive byte for byte.
- No font, shader, constant-buffer, Vulkan resource or render-path changes.
- Runtime translation strings are immutable after first load; cached labels have stable
  storage and a mutex, with a defensive entry cap and English fallback.
