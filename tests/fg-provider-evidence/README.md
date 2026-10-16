# DLSS-G provider evidence checks

Run `python3 tests/fg-provider-evidence/run.py` with Python's standard library.

This executes the real provider runner's evidence judge on complete synthetic
records and requires it to reject missing interpolation surplus, a missing real
plugin oracle, incomplete fence/frame records and incorrect enable state.
The records test the judge; they provide no GPU or DLSS-G runtime evidence.

The full runtime experiment lives in `tests/fg-provider-d3d12` and needs the
production DLL, installed NVIDIA assets, MSVC, Proton and a graphical GPU session.
