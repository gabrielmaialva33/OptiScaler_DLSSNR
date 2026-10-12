# HDR highlight recovery: NVIDIA's mask over the composed frame

Status: **implemented, opt-in, not yet judged in a game.** `[DlssNr] HighlightRecovery=false` by default.
Shader behaviour measured on a real GPU through vkd3d-proton (§5.1), which is not a D3D12 validator.

Line numbers on our side are against `758db3bc`, the parent of this change. On NVIDIA's side they are
against NVIDIAGameWorks/dxvk-remix `ecf646d3ea` (2026-09-18, "DLSS 3D-Guided Neural Generation
Integration"), the commit that added DLSS-NR to RTX Remix.

## 1. Where this comes from

The research pass of 2026-10-12 (`~/.local/state/dlss5/notes/research-20261012/nvidia-side.md`,
"Placement, input range and resolution" and "What to try first", item 3) found that RTX Remix wraps the
model in a tonemap, the model, an inverse tonemap, and a **highlight recovery** step. It ships that step
switched on, with fixed smoothstep thresholds. This note answers whether our pass needs the same thing,
and ports it where it does.

## 2. What Remix does

### 2.1 Order

`src/dxvk/rtx_render/rtx_context.cpp`:

- `:725`: DLSS upscales into `m_finalOutput`. That is linear HDR at display resolution.
- `:751`: `dispatchDlssNR`. It runs before bloom (`:753`) and before the real tonemapper (`:758`), so
  the frame it hands on must still be linear HDR.
- `:1816-1847` (`dispatchDlssNR`):
  1. auto exposure (`:1826`);
  2. `dispatchFastToneMapping` (`:1831`), from `m_finalOutput` into `m_neuralRenderingInput`;
  3. the model (`:1838`), from `m_neuralRenderingInput` into `m_neuralRenderingOutput`;
  4. `dispatchInverseToneMapping` (`:1839`), writing back into `m_finalOutput` in place. This step runs
     only when the model succeeded.

So the order is: tonemap, then NR, then the inverse with highlight recovery, all in one shader.

### 2.2 The forward tonemap

`src/dxvk/shaders/rtx/pass/tonemap/tonemapping_fast_tonemapping.comp.slang:48-55`:

```
inputColor = sanitizeRadiance(hdr)                       // clamp to [0, float16Max], NaN -> 0
exposure   = sanitizeExposure(getExposure(...))          // exposureFactor x auto-exposure texel
ldr        = linearToGamma(ACESFilmApproximation(inputColor * exposure))
```

- `ACESFilmApproximation` is Narkowicz's fit, saturated: `tonemapping.slangh:81-89`.
- `linearToGamma` is a plain `pow(x, 1/2.2)`: `src/dxvk/shaders/rtx/utility/color.slangh:90-97`.
- `getExposure` (`tonemapping.slangh:33-41`) is `exposureFactor`, times the auto-exposure texel when
  auto exposure is on.
- `exposureFactor = exp2f(exposureBias() + calcUserEVBias())`: `rtx_tone_mapping.cpp:382`.

### 2.3 The inverse and the recovery

`src/dxvk/shaders/rtx/pass/tonemap/tonemapping_inverse_tonemapping.comp.slang`:

| Lines | What |
|---|---|
| `:60-64` | the model's answer, saturated and decoded with `gammaToLinear` (`pow 2.2`) |
| `:67` | `originalHdr`: the pre-NR frame, read from the in-out target before this thread overwrites it |
| `:69` | **`toneMappingInput = originalHdr * exposure`: this is what "exposed HDR" means** |
| `:70-71` | `inverseScalar = toneMappingInput / max(ACES(toneMappingInput), 1e-6)`, per channel |
| `:76-82` | `output = nr * inverseScalar / exposure`, or `originalHdr` where the scalar is invalid |
| `:89-106` | highlight recovery (below) |
| `:108-111` | non-finite output falls back to `originalHdr`, then clamp to `[0, float16Max]` |

The recovery, verbatim in substance:

```
originalLdr    = linearToGamma(ACES(toneMappingInput))           // what the model was shown
hdrIntensity   = max3(toneMappingInput)                          // exposed HDR, max channel
shoulderSignal = max3(originalLdr)                               // LDR, max channel
mask   = smoothstep(t.x, t.y, hdrIntensity) * smoothstep(t.z, t.w, shoulderSignal)
output = lerp(output, originalHdr, mask)
```

- The thresholds `t` default to `(1.0, 8.0, 0.75, 0.99)` with the step on:
  `src/dxvk/rtx_render/rtx_dlss_neural_rendering.h:78-83`.
- They are pushed per dispatch (`rtx_tone_mapping.cpp:383-388`) through `FastToneMappingArgs`
  (`tonemapping.h:121-132`).
- They are editable under "Advanced Settings": `rtx_dlss_neural_rendering.cpp:240-244`.
- `orderedSmoothstep` (`:36-46`) only orders user-supplied edges and handles equal ones.

**What it protects against.** Remix's inverse is a per-channel *multiplicative* scalar, `x / ACES(x)`:

- It is 2.2 at x = 2, 4.1 at x = 4, and 8 at x = 8. ACES saturates at about x = 7.24.
- An LDR edit in the shoulder is therefore multiplied by that slope on the way back. A model that darkens
  a blown pixel by 10% in LDR darkens the HDR highlight by 10% of a value that may be many times paper
  white.
- The model also cannot answer above 1.0.

The recovery throws the model's answer away on exactly those pixels, the very bright ones in the
shoulder, and puts the original HDR back.

Remix's own mask over its own curve, neutral grey at exposed value x:

| x | ACES (linear) | LDR | mask |
|---|---|---|---|
| 1 | 0.804 | 0.905 | 0.000 |
| 2 | 0.915 | 0.960 | 0.053 |
| 3 | 0.954 | 0.979 | 0.197 |
| 4 | 0.973 | 0.988 | 0.393 |
| 6 | 0.993 | 0.997 | 0.802 |
| 8 | 1.000 | 1.000 | 1.000 |

The shoulder term is already near 1 by x = 2, so the `preserve` term on the max channel decides the
mask almost everywhere.

## 3. What our pass does with highlights today

`OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`, default configuration: `ReversibleMode=0`, detail 1,
colour 1, guard 2. This is Crimson Desert's: "colour transform on (linear HDR), transfer matched
residual, guard 2.0x", `WhitePointSource=1`.

### 3.1 The encode: where the energy leaves what the model sees, on purpose

| Lines | What |
|---|---|
| `:746`, `:752` | `frame = max(source, 0)`. The kept copy is the frame value for value, signs included |
| `:778` | `normalized = frame / WhitePoint()` |
| `:252-264` | `WhitePoint()`, under source 1 on D3D12, recomputed in the shader as `gExposurePreMul / exposure` |
| `DlssNr_Dx12.cpp:4187-4200` | `gExposurePreMul = PreExposure x trim` |
| `:365-397` | `SoftKnee`: luminance above 0.75 rolls toward 1, then a peak channel above 1 divides the whole triple (`:391-394`) |
| `:792`, `:273` | `LinearToSrgb` saturates before encoding |

From that:

- **Our `normalized` is Remix's exposed HDR.** It is `O x exposure / (PreExposure x trim)`, the game's
  own exposed value: the number the game's tonemapper takes, divided by the trim. That is the same
  quantity as `toneMappingInput` in §2.3, up to the trim.
- **With the soft knee, everything above paper white is squeezed into a thin band at white.**
  - A saturated pixel's peak is divided down to exactly 1 (`:391-394`).
  - A near-neutral one is rolled by the luminance knee first, to 0.908 at 1x and 0.998 at 2x.
  - Hue is kept, because one scalar is applied to the whole triple.
  - "Scene 2 and scene 4 arrive ~0.001 apart" (`:403-404`).
- Neutwo and Hybrid (`:415-477`) do not clip, but still compress toward 1: 1x lands at 0.707 and 0.927.
- The model answers in sRGB `[0, 1]`, and the resolve decodes it with a saturate (`:279`, `:849`). The
  model cannot express anything brighter than white.

### 3.2 The resolve: how the energy comes back

Pixels in the resolve:

- `original = max(O, 0) / W` (`:873-875`).
- `proxy` is what the model was shown, decoded (`:848`).
- `model` is its answer, decoded.

**Composition (`:1025-1063`).**

- `SoftKnee` never raises a value, so for every pixel above paper white `proxyLuma <= originalLuma`, and
  the resolve takes the **headroom branch** (`:1045-1051`):
  `ratio = (mY + oY - pY) / mY`.
- `HueOkLab(model * ratio, model)` (`:1062`) therefore lands at luminance `oY + (mY - pY)`.
- **The original's headroom is carried additively. It is not multiplied by an inverse slope.** A model
  that darkens a highlight by Δ in proxy units (Δ ≤ 1) takes Δ off an HDR value of `oY`. Remix's
  multiplies Δ by `x / ACES(x)` instead.

**The guard (`:1115-1151`).**

- The room to darken, `drop` (`:1146`), is `lerp(1/guard, 1, smoothstep(0.6, 1.1, originalLuma))`.
  **Above 1.1x paper white in luminance, no pixel can come out darker than it went in, whatever the
  model returned** (`:1140-1145` says so).
- The room to brighten, `lift` (`:1131`), is the full guard for anything but near-black.
- This floor is the highlight protection our pass already has. It is keyed on **luminance**.

**Colour (`:1187-1193`).**

- At colour strength 1, the model's hue and chroma are taken.
- The only bound is `kColourBound = 2.0`: a colour swing of up to twice the pixel's own luminance passes
  whole.
- `hueTrust` (`:1168`) is 1 for any lit highlight.

**Replace modes (`:1202-1205`).** `ReversibleMode` 2 and 4 take `NeutwoDecode` / `HybridDecode` of the
model's answer, with **none** of the above.

- That is Remix's shape exactly: a curve, the model, the exact inverse. Its inverse is steeper than
  Remix's: Neutwo's at the clamped ceiling is about 707 x W.
- `model-cadence.md` Addendum A works through how a highlight flashes there.

### 3.3 Matched residual

`:969-1018`. `fullProxy = saturate(SoftKnee(original))` is the frame's own proxy at full resolution, so
its peak channel is 1 on any highlight. `CubeScaleResidual(fullProxy, fullProxy + edit)` (`:514-531`)
then works as follows:

- An edit with a **positive** component on a channel already at 1 gives `alpha = 0`, and the pixel's whole
  edit is discarded. The composition then returns the original exactly.
- A **negative** edit passes, and the guard's floor takes it back out above 1.1x paper white.

So **yes**: matched residual re-applies the model's edit on top of the frame's own proxy, and the
composition carries it onto the original HDR with the highlight's headroom taken from the original. In
luminance, that preserves highlights. **In chroma it does nothing that Classic does not.**

**In Crimson Desert matched residual is not what runs.**

- Its ini has `WorkingScale=1.0`, and its log shows `model 3440x1440` against a frame of 3440x1440.
- The residual path is taken only when the model ran below the frame (`modelRanSmall`, `:971-975`), so
  the shader runs Classic.
- The log's "transfer matched residual" prints the configured `Transfer`, not the path taken
  (`DlssNr_Dx12.cpp:4989`, `:5094-5105`).
- Both end in the same composition, so the analysis above holds either way.

### 3.4 Worked numbers

The figures below come from a scratch port of the resolve's Classic path (`:1025-1193`), mode 0, guard 2.

- `o` is the frame divided by the white point.
- "Model x0.8" means the model returned its proxy at 80%.
- "Model -> white" means it returned a neutral at the proxy's peak.

| Pixel `o` | Y | proxy | drop floor | Y out, model x0.8 | Y out, model x0.5 | Model -> white |
|---|---|---|---|---|---|---|
| neutral (4, 4, 4) | 4.00 | (1, 1, 1) | 1.00 | x1.000 | x1.000 | x1.000, unchanged |
| fire (8, 4, 1) | 4.63 | (1, .5, .125) | 1.00 | x1.000 | x1.000 | **x1.091, (5.05, 5.05, 5.05): the fire turns white** |
| red light (4, .1, .05) | 0.93 | (1, .025, .012) | 0.86 | x0.950 | **x0.875** | **x1.83, (4.16, 1.03, 0.99)** |
| blue light (.2, .5, 6) | 0.83 | (.033, .083, 1) | 0.73 | x0.967 | x0.917 | **x2.00 (the guard), (0.81, 1.21, 8.7)** |
| blue light (.1, .2, 10) | 0.89 | (.01, .02, 1) | 0.80 | x0.980 | x0.950 | **x2.00, (0.50, 0.67, 16.5)** |
| neutral (1.5, 1.5, 1.5) | 1.50 | (.988, .988, .988) | 1.00 | x1.000 | x0.999 | x1.000 |

### 3.5 Verdict

**Where a highlight above paper white can still be dimmed or flattened:**

1. **Saturated highlights whose luminance is under 1.1.** The guard's floor is keyed on luminance, and
   luminance weighs blue at 7% and red at 21%. A light whose max channel is four to ten times paper white
   can sit at Y ≈ 0.85.
   - There, the composition can dim it by 5-12% (the table's red and blue rows).
   - It can brighten it up to the guard's 2x while pulling it toward the model's colour.
2. **Colour, on every highlight.** The model judged a pixel whose brightness it was never shown: its peak
   was pinned at 1 by the encode. Its colour verdict is then applied whole, under a bound of twice the
   pixel's luminance.
   - "This blown orange is white" turns an 8x fire core white, and 9% brighter.
   - The game's own tonemapper has not run yet: we sit before it, in linear light. A whitening made here
     is made again there.
3. **The replace modes.** They have no composition and no guard (`:1202-1205`).

**Where it cannot be:**

- In luminance, in the composed modes, on any pixel at or above 1.1x paper white: the floor is 1, and
  the headroom term is additive. This is the failure Remix's recovery exists for, and our pass
  structurally does not have it.
- A neutral or luminous highlight comes back at its own luminance, whatever the model did.

**So the gap is real, but narrower than Remix's: colour and saturated highlights, not energy.** That
justifies a port, opt-in. It does not justify a default change before it has been judged in a game.

## 4. The port

### 4.1 The step

The recovery goes in the resolve **after the replace decode and before the frame leaves normalised space**:
between `dlssnr.hlsl:1205` and `:1209` of the parent. In that position:

- the guard, the colour bound and the colour boost have already acted;
- the replace modes have already decoded;
- the `showOriginal` / letterbox / divider overrides come after it, so they are unaffected.

The step:

```
if (gHighlightRecovery != 0 && gPassthrough == 0)
    hdrIntensity   = max3(original)                       // original = max(O, 0) / W: exposed HDR
    if (hdrIntensity > 1)                                 // smoothstep(1, 8, x) is 0 below 1
        shown          = LinearToSrgb(encodeCurve(original))   // what the encode showed the model here
        shoulderSignal = max3(shown)
        mask   = smoothstep(1, 8, hdrIntensity) * smoothstep(0.75, 0.99, shoulderSignal)
        mask   = finite(mask) ? mask : 0
        result = mask >= 1 ? original : lerp(result, original, mask)
```

**Remix's two inputs, in our terms:**

- **Exposed HDR** is `original`, `max(O, 0) / W` (§3.1).
- **LDR** is the encode's own curve applied to the pixel at the frame's size, sRGB-encoded: the
  encode's `:778-792` reproduced for this pixel. For the soft knee, `encodeCurve` is `SoftKnee`
  (passthrough-gated inside, and unreachable here anyway). For Hybrid and Neutwo it is their encodes.

  It is computed per pixel rather than read from the proxy texture. At a reduced working scale the
  texture is the downsampled proxy, which is not this pixel. Reproducing it is exact: Classic and the
  matched-residual rebuild already treat the encode as a pure function.

**Then:**

- `result` and `original` are in the same normalised units.
- `:1209` multiplies back by W and returns the signed rest.
- At mask 1 the pixel is therefore the frame, up to the `x / W * W` rounding the strength-zero comment
  (`:832-835`) describes.

### 4.2 Thresholds: fixed, Remix's

The port's mask over our curves, neutral grey at `o`:

| o | Remix's mask (ACES) | port, soft knee | port, Neutwo | port, Hybrid |
|---|---|---|---|---|
| 2 | 0.053 | 0.055 | 0.052 | 0.055 |
| 3 | 0.197 | 0.198 | 0.197 | 0.198 |
| 4 | 0.393 | 0.394 | 0.393 | 0.394 |
| 6 | 0.802 | 0.802 | 0.802 | 0.802 |
| 8 | 1.000 | 1.000 | 1.000 | 1.000 |

The mask tracks Remix's to within 0.002, on every curve we have. The shoulder term saturates early on all
of them, so `smoothstep(1, 8, max channel)` decides.

- Given that the exposed value means the same thing in both pipelines (§3.1), Remix's thresholds carry
  over **unchanged**, and there is nothing to rescale.
- They are compile-time constants in the shader, behind one switch.
- A four-edge vector is not a user control: Remix itself hides it under "Advanced".
- Exposing it would add four sliders for one behaviour (`DEVELOPMENT.md` §1, rule 3).

### 4.3 Constants

`uint HighlightRecovery` is appended **at the end** of both `DlssNrConstants` (`DlssNr_Common.h`) and the
`Params` cbuffer (`dlssnr.hlsl`) (rule 5).

- The list grows from 24 to 25 four-byte scalars, from 96 to 100 bytes; the new field sits at offset 96 in
  both the C++ struct and the SPIR-V `Params` block.
- `alignas(256)` keeps `sizeof(DlssNrConstants) == 256`, so the constant-buffer view size is unchanged.
- The Vulkan slot stride is `sizeof` rounded up to the device alignment (`DlssNr_Vk.h:37`), and is unchanged
  too.
- Zero-initialised everywhere (`DlssNrConstants x {}`), so every caller that does not set it, the
  loopback harness included, is off.

### 4.4 Config, host, menu, log

**Config.** `[DlssNr] HighlightRecovery`, a bool, default `false`, round-tripped at all four points
(rule 4): declared in `Config.h` beside `MaxRatio`, read and saved in `Config.cpp`, and shipped in
`OptiScaler.ini` with a comment.

**D3D12.** `resolveParams.HighlightRecovery = isHdrBuffer && key` in `DlssNr_Dx12::Dispatch`.

- That is the only resolve, and every D3D12 route reaches it: after the upscaler, before it (Stage 1),
  the present pass, and the D3D11 and Vulkan-on-D3D12 bridges.

**Vulkan.** `encode.HighlightRecovery = linearHdr && key` in `DlssNrFeature_Vk.cpp`. The resolve's
constants are a copy of the encode's (`resolve = encode`).

**Menu.** A "Highlight recovery" checkbox directly under "Highlight guard".

- It is drawn only when the last dispatch's buffer was linear HDR, read from new accessors:
  `DlssNr::BufferIsLinearHdr()` for D3D12 and `DlssNr::LinearHdrVk()` for Vulkan.
- It is not drawn under `UseProxy`, where the D3D12 route returns before the resolve. The Vulkan route
  ignores `UseProxy`.
- Before anything has run, on an SDR or already-tonemapped buffer, and under the proxy, there is nothing
  for it to act on, so there is no control (rule 2).
- The flag is "the last dispatch", and is not cleared when the pass is switched off. The panel's other
  Colour controls stay visible then too.
- Its strings have pt-BR entries.

**GPU timing.** The timing contract gains ` highlight_recovery=1` only when the resolve is sent it: on,
over a linear HDR buffer. Samples either side of the toggle therefore fall into separate contracts, and the
default contract keeps its text and hash. This follows the pattern of `cadence=` and `periphery=`, and is
tested in `nr-timing-boundary`.

**Log.** The composition line gains `highlight recovery on|off`.

- It is part of `ComposeReport`, so a toggle is logged at once.
- The output stabilizer's history is invalidated by the same comparison (`SameShape`), as it must be
  for any setting that changes the composed picture.

### 4.5 Interactions

- **`gPassthrough` (rule 7).** The step is gated on `gPassthrough == 0`, and the host sends 0 whenever
  the buffer is not linear HDR. The encode reproduction inside it is also passthrough-gated
  (`SoftKnee`). An already-tonemapped frame is untouched two ways over.
- **Debug views 1-3, strength 0, `ApplyModel` off.** Each returns before the composition (`:836-840`,
  `:883-929`), so none reaches the step.
- **Compare.** The original side takes the frame after every step (`:1212-1213`). The edited side is
  recovered.
- **Before the upscaler (Stage 1).** The pass runs over the jittered render-size frame, and the resolve
  is the same one.
  - Recovered highlights hand the upscaler the game's own highlight pixels.
  - The step holds no temporal state, so there is nothing to reproject or reset.
- **Transfer 2 and 3, peripheral compression, cadence-carried frames.** All end in this resolve, so the
  step applies identically. The cadence records the model's answer, not the resolve's output, so recovery
  never feeds back into what it carries.
- **Colour boost above 1 (`:1195-1196`).** That runs before the step, so a recovered highlight does not
  keep the boost.
- **The present route.** Its frames are display-referred (passthrough), and HDR swapchains are declined
  (`DlssNr_PresentColour.h`). The step is off there.
- **Cost.** It is paid only with the switch on, and only by pixels above paper white: one curve
  evaluation, three `pow`, two `smoothstep`.

## 5. Risks

- **Native Windows, RTX 3060.** The step adds no descriptor, no root-signature entry and no resource. The
  constant buffer grows inside its 256-byte allocation, and the view size does not change
  (`nr-invariants` §5 checks the `alignas(256)`). There is nothing here that vkd3d-proton could accept and
  Windows refuse, but a run on Rafael's PC is still the bar before calling it done.
- **SDR buffers.** Gated off in two places, as above. On a UNORM buffer `o <= 1` anyway, which makes the
  mask 0.
- **A wrong paper white.** If W is too low, much of the frame sits above 1 and the step discards more of
  the model's edit than intended: a bright sky loses the model entirely. That is a symptom of the white
  point, not of this step. Crimson Desert's game-driven W runs at 6.5-8.6x in the session logged on
  2026-10-12.
- **It discards legitimate edits.** Detail the model draws on a bright cloud at 4x paper white is 39%
  undone. That is the trade, and the reason it is opt-in.
- **Seams.** The mask is a smooth function of the pixel's own value, with no hard threshold, so it draws
  no edges of its own. Across the rim of a bright window it fades the model's texture over the ramp.
  Look there.
- **Replace modes.** The mask reaches 1 only at 8x paper white, while the replace decode's slope is already
  steep near the curve's ceiling. That mitigates the flash and does not remove it.
- **Byte-identical when off (rule 1).**
  - With the switch off, the shader executes the code it executed before. The new block is skipped on a
    uniform branch, and nothing before it changed.
  - The bytecode changes, because the shader grew. Every appended constant has landed that way.
  - Shown on a real device rather than argued: §5.1.

### 5.1 Measured on a real GPU through vkd3d-proton (2026-10-12)

A throwaway harness, not committed, ran the shader's encode, downsample and resolve on vkd3d-proton
(GE-Proton11-7) on the RTX 4090, with the parent's bytecode and this branch's side by side.

- **Build and run.** It is built the way `tests/nr-gpu-timing-d3d12` builds, through a reflinked copy of
  the toolchain prefix, never the compiler prefix itself, and run in its own Wine prefix with no NGX.
- **Bindings.** As `DispatchPass` binds the pass: five SRVs, two UAVs, the constants and the linear clamp
  sampler.
- **Fixture.** A 256x128 RGBA16F frame with a white point of 2: a neutral ramp from 1/128x to 11x paper
  white, eight saturated hues rising to about 10x, noisy tinted texture, and negative channels, zeros and
  values up to about 56x.
- **The "model".** It whitens anything near white, textures everything, darkens a band and leaves a block
  empty.

Results:

- **Off is byte-identical.** 380 of 380 resolve configurations match the parent's output half for half.
  - The configurations cover `ReversibleMode` 0-4, `Transfer` 0-3, the model at full and half size,
    linear and passthrough, and detail/colour at 1/1, 2/2, 0.5/0 and 0/1.
  - They also cover debug views 1-3, both compare modes, and `ApplyModel` off.
  - The encode and the downsample match too.
- **On, where it must not act, nothing moves.** 190 passthrough configurations are identical to off, and so
  are 60 early-out ones (strength 0, a debug view, `ApplyModel` off).
  - 1,994,160 pixel checks at or below paper white are identical to off.
- **On, at and above 8x paper white, the pixel is the frame bit for bit.** That is 657,600 pixel checks,
  including channels that are negative.
  - This holds for this fixture: an RGBA16F output and a white point of 2, a power of two.
  - In general a recovered pixel is the frame up to the `x / W * W` rounding of §4.1. That is one fp32 ulp,
    which an FP16 or R11G11B10 output absorbs, and which can show on an R32G32B32A32 buffer with an
    arbitrary W.
- **In between, it is Remix's blend.** In 1,280,400 pixel checks the output is `lerp(output when off,
  frame, mask)`, with the mask computed on the CPU from §4.1. The worst relative error is 9.6e-4, which is
  half-float rounding.
- **The comparison is not vacuous.** With the switch on, 1,513,645 pixels changed, so the same comparison
  that found nothing with it off does see a difference when there is one.

What this does not show: how it looks in a game, its cost, and anything about the Vulkan SPIR-V beyond its
layout. `spirv-val` passes, and the member offsets match the C++ struct.

## 6. Attribution

The recovery is NVIDIA's: the max-channel smoothstep on the exposed frame, times the smoothstep on what
the model was shown, blended back toward the original, with the thresholds `(1, 8, 0.75, 0.99)`. It is
reimplemented in HLSL. The ACES wrapper around it is **not** ported, because our encode is our own.

- The three shader files carry NVIDIA's per-file MIT-style grant ("Copyright (c) 2022-2026, NVIDIA
  CORPORATION").
- The repository's `LICENSE` is dxvk's zlib/libpng.

`Licenses/RTXRemix_ATTRIBUTION.txt` carries both texts and says which part is theirs.

## 7. How to judge it in Crimson Desert

1. **Menu.** Neural Rendering, Colour section, the "Highlight recovery" checkbox under "Highlight
   guard". It shows only once the pass has run on the game's linear HDR buffer.
2. **Log.** `DLSS-NR composition: ... guard 2.0x, highlight recovery on, colour transform on (linear
   HDR), ...` must appear when it is ticked, and `off` again when it is cleared.
3. **A/B.** Turn on "Hold frame", then toggle the checkbox: the same frozen frame is re-composed each
   way. Or use the compare wipe, with the checkbox on, against the original.
4. **Scenes, where the gap is:**
   - fire and torches at night, whose cores should keep their orange instead of going white;
   - saturated coloured lights: blue magic effects, red lanterns;
   - the sun disc and sky around it, and specular glints on water and armour.
5. **What to look for:**
   - highlights keeping the game's own colour and brightness, with the model's look intact below paper
     white;
   - no edge or halo where the mask ramps, around a lamp or the rim of a bright window;
   - nothing changing in shadows and midtones.
6. **Result.** If colour in highlights reads better with it on and nothing else moves, it is a
   candidate default. That is a separate change, measured first.
