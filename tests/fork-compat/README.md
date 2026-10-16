# Fork compatibility regressions

Run `python3 tests/fork-compat/run.py` on the host with Python and g++.

This suite slices the real production code into two small executables, compiled
with AddressSanitizer and UndefinedBehaviorSanitizer:

- `Config.cpp`'s XeSS/XeFG/XeLL read/save statements and `readWString`, with
  `Config.h`'s real `CustomOptional` and SimpleIni. Distinct paths and individual
  overrides must stay independent through read, save into an empty INI, and read
  again. Missing keys and case-insensitive `auto` remain unset.
- Both signature hook bodies (`Crypt32_Hooks.h`, `Wintrust_Hooks.h`) against
  minimal Win32/COM-boundary fakes. Only the case-insensitive basename
  `nvngx.dll` is redirected: the driver's `_nvngx.dll`, the NR forwarder,
  similarly named files and directory names pass through. Also checks existing
  spoofing/replacement gates, non-file/empty inputs, trust-state propagation and
  FFX handling.

The tests do not call Windows trust providers, load a DLL, or run NGX/a game.
