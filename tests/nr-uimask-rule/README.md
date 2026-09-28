# nr-uimask-rule

The per-pixel rule of DLSS-NR's HUD-protection mask, `OptiScaler/shaders/dlssnr/precompile/dlssnr_uimask_rule.h`,
is the code the shader (`dlssnr_uimask.hlsl`) includes. This suite includes the same header in C++, with
the shader's texture loads replaced by array lookups that clamp the same way, and runs it over synthetic
frame sequences exactly as the shader does: last frame's state in, next frame's state out.

Cases:

- **Glyph over a pan.** A bright glyph with a dark 2 px outline over grainy scenery panning 3 px a
  frame. Nothing may be protected before the entry streak (8 frames). After it, the glyph's inner
  edges must be, and nothing outside the glyph and its 1 px growth may be.
- **Silhouette on still scenery.** A dark coat sways ±3 px over still, grainy sand, as an idle
  animation does. The first slice's thresholds must protect a rim of sand; this is what the PCSX2 A/B
  showed as a golden outline, and it proves the fixture reproduces the bug. The shipped thresholds
  must protect none.
- **Concave gap.** Sand between two swaying legs under a swaying coat, moving on three sides: not
  protected.
- **Drop on change.** A protected glyph vanishes into the scene. Every pixel whose own change exceeds
  `DropTau` must be unprotected on that very frame.

Run: `python3 tests/nr-uimask-rule/run.py` (g++, ASan/UBSan). What it does not cover: the texture
formats, the dispatch and the model's response to the mask. For the last one, see
`tests/dlssnr-loopback/run.py --present-nr --ui-protect-ab`.
