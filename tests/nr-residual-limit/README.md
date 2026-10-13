# nr-residual-limit

Host suite for the residual limit (`[DlssNr] ResidualLimit`, `docs/dlssnr/design/residual-limit.md`): a flat bound,
in stops, on how far the model may move a pixel's light, applied in the composition after the highlight guard.

Run: `python3 tests/nr-residual-limit/run.py` (needs `g++` with C++20, ASan and UBSan).

`run.py` slices the guard and the limit out of `retoque/shaders/dlssnr/precompile/dlssnr.hlsl` -- the shader's own
text, from `const float guard` to the limit's clamp -- and compiles it twice into `test.cpp` against C++ versions of
HLSL's intrinsics (`clamp` as `min(max())`): whole, and cut before the limit. `test.cpp` holds the two against each
other over 5 guards x 12 luminances x 97 ratios from 1/16 to 16:

- off is the guard alone, to the bit, for 0, -0, a negative limit and NaN;
- on, every limit from 0.05 to 2 stops bounds the ratio both ways, only moves it toward 1 (never further than the guard
  left it, never across 1), leaves a ratio already inside the limit untouched, stays inside the guard's own floor and
  ceiling, and is symmetric in stops;
- the host's `DlssNr::ResidualLimitStops` sends only 0..2, off for anything not above 0, and zero-initialised
  constants are off.

`run.py` also guards by source: the shader reads the constant in one clamp only, appended last in the cbuffer, after
the guard and the strength-zero return and before the correction, the colour blend and the replace curves; D3D12 and
Vulkan send the clamped value; the composition report carries it (so a change logs at once and resets the
stabilizer) and the pulse metric's shape does too; the menu hides the control under the replace curves and the D3D12
driver proxy.

What it does not cover: dxc's compile of the shader, the GPU's float behaviour, and whether a value removes blotches
without removing detail. The design note's in-game check is for those.
