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

## Deliberately partial coverage

This first pack translates NR status, section text and selected main-control explanations.
It includes Portuguese terminology for the main controls and menu shell, but interactive
labels are only translated where the original already contains a stable `###` identifier.
Labels with no hidden ID, or only `##`, **remain in English**, including the existing main
NR control labels, combo options and most shell controls. Their translated explanations help
interpret those controls without changing their interaction or stored ImGui identity.
Dynamic diagnostic reasons and text absent from the dictionary fall back to English.
This is not a fully translated menu, nor a runtime language switcher.

The conservative label rule is intentional: ImGui hashes the `###` marker itself. Simply
turning `Reset` into `Redefinir###Reset` changes the original identifier; retaining only
`##id` also changes it. Neither is used here. Existing `###` suffixes are preserved exactly.
The test suite checks actual menu labels against the repository's real `ImHashStr`.

The supplied Portuguese text uses glyphs in the existing default Latin range. No Chinese
font loading, font atlas rebuilding, automatic locale selection or shader change is imported.
Actual on-screen layout/glyph rendering remains an integration check.

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
