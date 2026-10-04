# Optional Portuguese (Brazil) text pack

The package ships `optional/pt-BR.lang` **inactive**. The menu only loads a file named
`OptiScaler.lang` directly beside the OptiScaler DLL, once per process. It does not search
this directory, inspect the system locale, or select a font. Without that file, every lookup
returns the original English string. Unknown entries also stay in English.

To prepare activation, copy `optional/pt-BR.lang` to `OptiScaler.lang` beside the DLL in an
**isolated staging directory**. This instruction does not install anything into a game.
Only deploy that staged selection as part of an explicitly authorized game test. Restart the
application after changing or removing the active dictionary. Remove/rename `OptiScaler.lang`
and restart to return to English. The optional directory can safely remain in place.

## What it covers

The pack covers the DLSS Neural Rendering panel, synthesized frame generation's settings, the
section headers and menu shell already routed through `Localization`, and the status and failure
reasons the NR panel shows. Since upstream's menu redesign (merged 2026-10-04) the shell is translated
in a few central places rather than at each call site: `SectionTitle` and `ScopedCard` (every section
and card title), the sidebar's tab and group names, and the Custom tab picker. The sidebar widens to
fit the longest translated name. The rest of upstream's menu is untouched and stays in English: routing
its strings would change most of `menu_common.cpp`, an upstream file, and every merge from
`optiscaler/master` would conflict on it. Dynamic diagnostic text absent from the dictionary also
falls back to English. This is not a runtime language switcher.

English is the default and is unchanged without the file: `Tr` returns the original pointer and
`Label` the original string, byte for byte. Filter names (FSR1, Lanczos3, ...) are kept in English
and listed as themselves, because every production label must have an entry (see the tests).

## Labels and ImGui identity

ImGui identifies a control by hashing its label, and `ImHashStr` restarts at `###`, so everything
after it is the control's whole identity. `Label` keeps that identity stable under translation:

- a label that already has `###` keeps it: `Reset###fixed` becomes `Redefinir###fixed`;
- a bare or `##` label gets its whole original text after a new `###`: `Reset##detail` becomes
  `Redefinir###Reset##detail`, and `Enable Neural Rendering` becomes
  `Ativar renderização neural###Enable Neural Rendering`.

The second case changes the control's ID from `hash(original)` to `hash("###" + original)`. That is
safe here: the pack loads once per process, so the ID is fixed for the whole run, and nothing stores
ImGui IDs across runs (`menu_common.cpp` sets `io.IniFilename` to null). Two labels share an ID after
translation exactly when their English originals did, so no two controls merge; the suite checks that
over every production label. Until 2026-09-28 bare and `##` labels stayed in English to keep the
original hash, which left most of the panel's controls untranslated.

## Dictionary format

UTF-8 escaped TSV, one exact English source and one translation separated by one literal tab.
Blank lines and lines starting with `#` are ignored. An initial UTF-8 BOM and CRLF are accepted.
Use `\n`, `\t`, `\r` and `\\` inside fields. Duplicate keys, bad UTF-8, NUL/control bytes,
unknown escapes, extra tabs, empty translations, injected `##`, incompatible printf formats,
`%n`, positional formats and oversized files/lines reject the entire dictionary.

Keep format directives exactly in order, including width, precision, `*`, length modifiers
and `%%` literals. This intentionally rejects harmless-looking format changes as well as
unsafe ones. Unsupported printf dialects or bare `%` characters cannot be translated in this
first parser; those English strings may simply be omitted from the pack.

The optional-dictionary idea was inspired by Moeblack's localization work in
[PR #26](https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/26). The loader, validation and ID
policy here are a separate implementation adapted to this repository's invariants.
