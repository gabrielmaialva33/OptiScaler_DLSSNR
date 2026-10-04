# Peripheral compression: the model's input denser in the centre than at the edges

Status: **designed 2026-10-04; D3D12 v1 built behind `[DlssNr] PeripheryCompression`, default off;
host-tested, not yet run in a game or on native Windows.** The pack/unpack bytecode is optional: a
build without it says "unavailable" and runs exactly as before.

## Where this comes from

- **The mapping is BeliyG3's.** Yuri Grib's PeripheralWarp, in
  [optimizer-fps-dlss5](https://github.com/BeliyG3/optimizer-fps-dlss5) at `64902dd6` (MIT). Its
  radius curve, its inverse and the per-axis budget split (`BuildAxis`) are ported verbatim; the MIT
  notice travels with them and `Licenses/PeripheralWarp_LICENSE.txt` carries the full text.
- **The integration shape is wilsjo2's.** [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
  v0.8.91 (`be229250`, GPL-3.0) packed the model's colour, depth and motion and unpacked the answer
  before its composition. The extent rules (`EvenExtent`, the validity checks, the one-texel rule) are
  ported from its `DlssNr_Spatial.h`. Two of its defects are not (below).
- Nothing else is copied. This tree's pass has a different shape, so the integration is re-derived
  and the rules are pinned by `tests/nr-periphery`.

## The idea

At equal cost, spend the model's pixels where the eye is. The centre band keeps the uniform path's
density; the periphery is squeezed with a smooth shoulder so the model works on fewer pixels in all.

Per axis, around the centre, with `r` the distance from the middle over that side's half-span (`r = 1`
is the frame edge), `c` the centre band and `w` the work extent (both as fractions of the half-span),
and `k = (w - c) / (1 - c)` the compression:

| `r` | packed radius |
|---|---|
| `r <= c` | `r` (unchanged) |
| `c < r <= 1` | `c + (w - c) t / (k + (1 - k) t)`, with `t = (r - c) / (1 - c)` |
| `r > 1` | `w + (r - 1) k^2` (a straight line past the edge, for motion that leaves the frame) |

The slope is `k^2 / (k + (1 - k) t)^2`: exactly 1 at the band edge and exactly `k^2` at the frame edge,
which is the slope of the line beyond it. So there is no kink at either join, the curve is strictly
increasing, and its inverse is closed-form. The packed image is that radius times the half-span times
the global scale (`model / raw work`), around a packed centre.

**What the defaults cost.** `PeripheryCenter=80`, `PeripheryWork=90`: `k = 0.5`, the model sees 81
percent of the uniform path's pixels -- exactly what a uniform `WorkingScale=0.9` costs -- and one packed
pixel at the frame edge covers 4 native pixels at 100 percent (8 at 50 percent). The centre 80 percent
of each axis is at full density.

| Frame, Model resolution | Uniform grid | Packed model | Edge footprint |
|---|---|---|---|
| 3440x1440, 100 | 3440x1440 | 3096x1296 | 4 px |
| 3840x2160, 100 | 3840x2160 | 3456x1944 | 4 px |
| 1920x1080, 50 | 960x540 | 864x486 | 8 px |

## How it runs (D3D12, `DlssNr_Dx12::Dispatch`)

Everything below the encode changes size; nothing about the frame does.

1. **Layout.** `DlssNr::Periphery::Build` turns the two settings, the frame and the working scale into
   two axes and a packed model extent. `workWidth`/`workHeight` in `Dispatch` become that extent, so
   the model, its output, the chain's ping surface, the settle gates, the chain's admission and the
   format-flip set all follow it without further edits. The uniform grid (what `WorkingScale` alone
   would give) is kept beside it for the unpack.
2. **Colour pack** replaces the downsample. Each packed texel is the **exact area average** of the
   frame's proxy over its warped footprint `[Unpack(i), Unpack(i+1)] x [Unpack(j), Unpack(j+1)]`:
   the same box integral as the downsample (mode 2 of `dlssnr.hlsl`, hhkbble's), with warped bounds.
   wilsjo2's 5-tap bilinear pre-filter is not taken. With nothing compressed `Unpack(i)` is
   `i * frame / model` and the integral is mode 2 exactly; the host suite holds the two together.
   Alpha is the centre texel's, as in mode 2.
3. **Guide pack** replaces the guide resample (`MatchGuides` and its rule do not apply while the
   periphery is on: the guides have to be in packed space whatever their size). Depth and motion are
   point-sampled at the packed texel's native centre. Motion is converted by moving **both ends** of the
   vector through the mapping, `Pack(p + mv) - Pack(p)`, so a vector shrinks where the periphery is
   squeezed and is unchanged in the centre. The model is then handed both at the packed extent, as a
   zero-origin region, with a motion-vector scale of **1.0** -- the vectors are already in packed pixels.
4. **The model and every extra pass** run on the packed triple. The UI mask, when on, runs on the packed
   input at the packed extent and is handed over there, so it stays consistent with what the model
   reads.
5. **Unpack, once, after the chain.** For each texel of the uniform grid, the packed proxy and the
   packed answer are sampled bilinearly at `Pack(native)` into two RGBA16F surfaces. The resolve then
   reads that pair exactly as it reads the uniform path's proxy and answer.
6. **Resolve with `ForceResidual`.** At 100 percent the uniform grid is the frame, so the resolve's
   `modelRanSmall` was false and matched residual never ran: the composition fell back to Classic over a
   proxy that had been through a pack and an unpack, which is the colour-shift case matched residual
   exists to fix. `ForceResidual` (appended to `DlssNrConstants` and the `Params` cbuffer) makes
   `Transfer` 1 and 3 take the residual path whenever the periphery is on. Classic (0) is still allowed;
   its help text says it blurs colour at the edges here.

## Two defects in wilsjo2's version, fixed here

1. **Motion scale in render pixels.** v0.8.91 multiplied the vectors by the game's raw `MV_Scale` and
   treated the result as frame pixels. For low-resolution vectors that scale is in render pixels, so at
   DLSS Quality every vector reached the mapping about 0.67x too short. Here the pack's scale is
   `ModelMotionScale(gameScale, frame, MotionReference(lowRes, render, output))` -- the game's scale
   re-expressed in frame pixels, the rule `reduced-scale-guides.md` already pins.
   `RenderMotionScale=false` (the legacy A/B) is not honoured while the periphery is on.
2. **Matched residual at 100 percent.** See step 6.

## Settings

| Key | Menu | Default | Meaning |
|---|---|---|---|
| `PeripheryCompression` | Peripheral compression | false | the whole feature |
| `PeripheryCenter` | Centre band | 80 | full-density centre band, percent of each axis (10 to 96) |
| `PeripheryWork` | Packed extent | 90 | packed extent before the working scale, percent of each axis (55 to 99) |

The controls sit under Model resolution, are hidden on native Vulkan, and the two sliders commit on
release. A status line says what the pass did on its last dispatch: active with the packed size, or
not active and why. `DebugView=4` ("Packed model input", offered only while the setting is on) shows the
model's packed input through the proxy view; without the periphery it is view 1.

The same two numbers serve both axes. wilsjo2 offers per-axis values, a centre offset and a budget
shift; v1 has none of them, so there is one quantity per control and no control that edits another's
value. `BuildAxis` still takes an offset and a shift (it is ported verbatim) and is always given 0.

**Validity**, each failure stated in the menu and logged once on change:

- the centre band is at least 10 and below the packed extent, and the packed extent is below 100;
- **compression at least 0.5**, i.e. `2 x Work >= 100 + Center`. BeliyG3 calls below that "may visibly
  alias" (`docs/CONFIG.md`); it also bounds the colour pack's footprint at 4 frame pixels per packed
  pixel at Model resolution 100, 16 at 25. A floor in v1, to be revisited once there is a measurement;
- the packed model at least a quarter of the frame on each axis;
- at least one raw work texel on each side of the band.

## Refusals in v1

The feature stays off, with the reason in the menu and one log line, on:

- **the before-upscale stage** -- a jittered render-size frame; nobody has looked at the mapping there;
- **supersampling** (`WorkingScale > 1`) -- the supersample legs are uniform resamplers;
- **`UseProxy`** -- the driver-core route has its own evaluate and no resolve;
- **a build without the pack/unpack bytecode**, or a pass that could not be built;
- **native Vulkan**: not implemented. The controls are hidden there. Vulkan has no guide resample yet
  either; doing both is the follow-up (it needs its own SPIR-V and more descriptor slots).

## History and rebuilds

- A change of the packed model extent is a resolution change: the feature, its surfaces and the
  matched guides are rebuilt, behind the existing 500 ms settle gate on the after-upscale route.
- A layout change at the same extent (centre only) resets the model's history and the stabilizer's.
- The sliders commit when released, like Model resolution.
- The unpacked pair is sized to the uniform grid, parked when that changes, and parked when the
  periphery is off, so the default path allocates nothing.

## The HUD

The UI mask's probes are 6, 14 and 24 packed pixels long. In the periphery those cover up to 4x more
native pixels at 100 percent (8x at 50), and most interface sits at the edges. The mask is still
correct -- it compares packed frames and protects packed texels -- but its notion of "nearby" grows
toward the edges. Watch for a HUD element near a corner losing protection, or scenery beside one gaining
it.

## D3D12 validity (Proton is not a validator)

- One constant block, `PeripheryConstants`, `alignas(256)`; its layout is pinned against the HLSL
  cbuffer by `tests/nr-periphery`.
- Created formats are typed: R32_FLOAT depth, R32G32_FLOAT motion (the matched-guide surfaces), and
  R16G16B16A16_FLOAT for the unpacked pair. The packed colour is the existing `colorSmall`, in the frame's
  format, as the downsample already writes it.
- Every descriptor in the table is bound for every dispatch, a static linear-clamp sampler, and the
  model's inputs in NON_PIXEL_SHADER_RESOURCE.
- **A Windows run is required before this is called working** (Rafael's RTX 3060).

## The deciding measurement

80/90 costs the pixels of a uniform `WorkingScale=0.9`. So the question is not whether it is faster
than 100 percent -- it is, by construction -- but whether it **looks better than 0.9 at the same cost**:
the centre at full density against everything at 0.9. If it does not, it is not worth having.

Predicted from the cost law in `model-cost-vs-working-scale.md`, not measured: about -0.85 ms on the
RTX 4090 at 3440x1440 and 100 percent, about -0.2 ms at 50 percent, about -2.3 ms on an RTX 3060 at
1080p and 50 percent. wilsjo2's -10.6 to -12.3 percent was a synthetic scene with zero motion on an
RTX 5090.

**Plan.** Crimson Desert, after-upscale, 3440x1440, `GpuTiming=true`, the same save and spot:

1. `WorkingScale=1`, periphery off -- reference.
2. `WorkingScale=0.9`, periphery off.
3. `WorkingScale=1`, `PeripheryCompression=true` at 80/90 -- expect
   `DLSS-NR periphery: active, model 3096x1296 ...` and `model motion scale 1.0 x 1.0`.

Compare 2 and 3 for detail in the centre, stability at the edges in a pan, and `model_ms`. Then the
Packed model input debug view, to see what the model was shown. Then Divinity on the D3D11 bridge at
0.5, and the same on Rafael's PC.

## What the log says

```
DLSS-NR periphery: active, model 3096x1296 for a 3440x1440 frame (uniform grid 3440x1440; centre 80, work 90, compression 0.50; 81 percent of the grid's pixels)
DLSS-NR working size: 3096x1296 at scale 1.00 (4.01 Mpx), frame 3440x1440 -- packed by peripheral compression
DLSS-NR periphery guides: depth and motion packed to 3096x1296; the game's vectors to frame pixels by 3440.0 x 1440.0 (game scale 2293.0 x 960.0 against 2293x960, render size, low-resolution vectors), then both ends through the mapping
DLSS-NR model motion scale 1.0 x 1.0: ... motion texture 3096x1296 (packed, vectors in packed pixels), model 3096x1296
DLSS-NR periphery: not active -- supersampling (Model resolution above 100) is not supported with peripheral compression
```

The third line is the one that shows the motion fix: with DLSS Quality and Ray Reconstruction off the
render size is below the frame's, and the frame-pixel scale must be the frame's width, not the game's.

## Not done in v1

Vulkan; per-axis values, offset and shift; the centre/work outlines over the frame; a periphery arm in
`dlssnr-loopback`.
