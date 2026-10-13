# nr-hdr-math

Host suite for the HDR present conversion's maths (`[DlssNr] HdrPresent`,
`docs/dlssnr/design/hdr-present-conversion.md`): the decode of an HDR10 or scRGB backbuffer into linear Rec.709 in
scRGB units, the encode back with the original as the reference, and the bypass that returns an untouched pixel as the
game's own code.

Run: `python3 tests/nr-hdr-math/run.py` (needs `g++` with C++20, `_Float16`, ASan and UBSan).

`run.py` slices the block between the markers in `retoque/shaders/dlssnr/precompile/dlssnr_present_hdr.hlsl` -- the
shader's own text: the ST 2084 transfer, the Rec.2020/Rec.709 matrices, the decode, the bypass rule, the texel the
encode writes -- and compiles it into `test.cpp` inside a namespace that gives it HLSL's intrinsics, beside the float64
reference in `retoque/dlssnr/DlssNr_PresentHdr.h`. FP16 (the work texture) is `_Float16`, round to nearest even.

In Python, before compiling:

- every constant of the shader's block equals the header's, and the ST 2084 constants equal their exact fractions;
- both matrices, in the header and in the shader, are the derivation from the BT.709 and BT.2020 primaries (D65) to
  1e-9, their rows sum to 1, and they are inverses to 1e-6;
- the shader decodes with one function for both passes, the encode goes through the bypass, alpha is the original's,
  and it binds two SRVs, one UAV and one cbuffer -- what the pass class gives it;
- source guards: the bytecode header is optional (`__has_include`), the root signature is 2/1/1, the constant buffer
  view is sized by an `alignas(256)` struct, typed UAV store is asked of the device, the D3D12 route makes the two
  textures in one place and only for an HDR frame (an SDR frame only parks them), the switch-off decision is the one
  it always was on both hosts, both D3D12 exits finish the timing sample before the list closes, every decline reason
  the menu's readout can show has a pt-BR entry (the localization suite cannot see them: they reach `Tr` as
  variables), and the new files are registered once each in the project and its filters.

In `test.cpp`:

- the shader's PQ decode against float64 over all 1024 codes (worst relative error measured at 5.1e-5, under FP16's
  own rounding);
- the PQ round trip: every code returns as itself in float64, in float32 and through FP16, with no bypass;
- the finest PQ code step from code 64 up (0.90%, near code 800) is more than four times the bypass slack (0.195%)
  and at least the output stabilizer's HDR10 unit (`kPqQuantRel`, 0.78%);
- Rec.2020 primaries decode to negative Rec.709 channels, kept, matching the reference;
- **HDR10, untouched**: every code on each channel and on all three, a grid of primaries and secondaries and 200,000
  random texels, through FP16 and through the composition's divide and multiply by four white points, come back
  **bit for bit** through the encode. The same originals re-encoded without the reference move in 42% of cases, by
  up to ~200 codes: saturated Rec.2020 colours lose a near-zero channel to FP16's rounding of the Rec.709 values it is
  the difference of. That is what the reference is for;
- the bypass hides no code for colours inside Rec.709. Outside it, its slack -- 0.2% of the pixel's light -- can be
  many PQ codes in a channel near zero; the suite reports how many and does not bound it;
- an edited HDR10 texel is encoded within one code of the float64 encode of the same edit;
- scRGB: every one of the 63,488 finite halves -- negatives, subnormals, values far above 1 -- comes back bit for bit,
  untouched or through the white point; the 2,048 NaN and Inf patterns are black to the model and come back as
  themselves;
- a non-finite processed texel keeps the original; a changed scRGB texel is stored as it is.

What it does not cover: dxc's compile of the shader, the GPU's `pow` (DXIL's exp2 and log2 are approximations), the
UNORM load and store, the dispatch, the barriers and the in-game picture. The wine-tier test the design names
(`tests/nr-hdr-d3d12`, the identity test on a real D3D12 device) does not exist yet; neither does the measured run on
an HDR display that would let the switch default on.
