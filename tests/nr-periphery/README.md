# nr-periphery

Peripheral compression's layout and mapping (`OptiScaler/dlssnr/design/peripheral-compression.md`):
`OptiScaler/shaders/dlssnr/DlssNr_Periphery.h` and the shared
`precompile/dlssnr_periphery_warp.h`, which the shader (`dlssnr_periphery.hlsl`) includes too. The suite
includes the same header in C++, in float, with the colour pack's texture loads replaced by a vector, so
it runs the shader's own arithmetic rather than a copy of it.

Run: `python3 tests/nr-periphery/run.py` (g++, ASan/UBSan, `-Werror`).

Cases (`test.cpp`):

- **Extents and refusals.** wilsjo2's smoke values (3840x2160 at 100 is 3456x1944, `Pack(192) = 64`,
  1921x1081 at 0.85 is a 1470-wide model) and ours: 3440x1440 at 100 is 3096x1296 and 81 percent of the
  grid, at 50 is 1548x648; 1920x1080 at 50 is 864x486. Every refusal with its reason: supersampling, the
  range, the compression floor (`80/89` refused, `70/85` taken), a model under a quarter of the frame,
  a frame too small, less than one texel on a side.
- **The mapping**, over eleven layouts including odd sizes and four centre/work pairs: the inverse to a
  hundredth of a pixel from 64 px before the frame to 64 px past it; strictly increasing; the packed
  extent tiles the frame; one-sided slopes at the band's edge and the frame's equal on both sides and
  equal to the analytic value (no kink); at 100 the band moves by a whole number of texels.
- **Nothing compressed is the uniform path.** With the work extent at the whole frame the mapping is a
  uniform scale, the guide read is `GuideMatch::PointSource` exactly at 2293 -> 1720 and 1920 -> 2688
  (and within one texel where a centre sits on a texel edge), and the colour pack is mode 2 of
  `dlssnr.hlsl` on a random image.
- **The colour pack's filter.** A constant frame packs to the same constant (the weights sum to one);
  the packed texels, weighted by their footprints, sum to the frame (nothing gained or lost); the edge
  footprint is bounded by `1 / (scale x k^2)` and the loop's span.
- **Motion.** Zero stays zero; unchanged in the band at 100, halved there at 50; shrunk toward `k^2`
  at the edge; finite with an end off the frame. And the frame-pixel scale that wilsjo2's v0.8.91
  got wrong: Cyberpunk's 2293 game scale against a 2293 render width is 3440 frame pixels, so a
  hundredth of the screen packs to 34.4, not the 22.93 the raw game scale gives.
- **Constants.** `PeripheryConstants` is 256 bytes; `MakeConstants` copies the layout.

Text guards (`run.py`): `PeripheryConstants` and the HLSL `cbuffer Params` are one ordered list;
`PeripheryAxis` is sixteen plain floats; mode 2 of `dlssnr.hlsl` still reads the way `test.cpp`
transcribes it; the shader has the three passes the C++ loads, under the names its build comment gives;
the PeripheralWarp attribution and `Licenses/PeripheralWarp_LICENSE.txt` are there.

Each guard was shown to fail before the suite was registered: a linear shoulder, the raw game scale,
no whole-texel snap, an area divided by the rectangle instead of the weights, the compression floor
dropped, and a rounded guide read were each injected into a copy of the headers and caught.

What it does not cover: the D3D12 dispatches, the formats, the model's response to a packed input, and
image quality. The bytecode is checked by `nr-invariants` once it is committed; whether it is the
compile of the current `.hlsl` needs dxc and stays with whoever builds the shader. In-game and native
Windows runs are in the design note.
