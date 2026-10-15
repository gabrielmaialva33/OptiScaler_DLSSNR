# NR present-guide identity under MSVC (wine)

```bash
python3 tests/nr-present-guide-identity-msvc/run.py
python3 tests/run_all.py --tier wine --only nr-present-guide-identity-msvc -v
```

The cases of `tests/nr-present-guide-identity` built by the compiler the DLL is built with. `decide.cpp` (the decision
header, step 1's record and line, the source-change reset, the status word and its text) and the renderer's glue,
sliced out of `retoque/shaders/dlssnr/DlssNr_Dx12.cpp` exactly as the host suite slices it and compiled after the same
fakes, are compiled by MSVC (`/std:c++20 /permissive- /W3 /MT`) in a private copy of the msvc-wine prefix under
`artifacts/`, and both executables run under Wine. The suite passes when each prints its `PASS` line, which it does
only after every check has held.

What it adds to the host suite: the header's arithmetic, string formatting, clock conversion and packing through MSVC's
standard library and code generation rather than g++'s. What it does not: AddressSanitizer and UBSan (the host suite
has them), and anything a real device, driver or game would show.

The first run copies the toolchain prefix (about 400 MB) into `artifacts/build-prefix`; later runs reuse it. Like the
other wine suites it refuses to start while a build or a shader rebuild has the shared prefix busy, and runs serially.
