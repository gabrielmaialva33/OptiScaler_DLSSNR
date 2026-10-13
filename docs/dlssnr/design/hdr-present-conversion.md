# HDR10 and scRGB at present: decode to linear, run the pass, re-encode with the original as reference

Status: design for review, not implementation, 2026-10-13. Revises the 2026-09 note of the same name.
Line numbers on our side are against `93730d39` (branch `docs/design-hdr-capture`); on upstream's and
NeuRotic's side against the commits named. Nothing here has been built or run.

## 1. The answer

Today the present-time pass declines every HDR swapchain (`retoque/dlssnr/DlssNr_PresentColour.h:26-69`)
and the game's own picture goes to the screen. This note turns that refusal into a conversion for two
profiles, HDR10 (R10G10B10A2, PQ, Rec.2020) and scRGB (R16G16B16A16_FLOAT, linear Rec.709), and keeps
everything else declined.

The decisions, each argued below:

1. **One working representation: linear Rec.709, FP16, in scRGB units (1.0 = 80 nits).** PQ is decoded
   into it; scRGB already is it. The existing pass then runs on it as a linear-HDR frame
   (`ColourIsLinearHdr = true`), which is the path it was built for. `dlssnr.hlsl` is not edited.
2. **Paper white is arithmetic, not a reading, and it is not a correctness parameter.** Default 203 nits
   (BT.2408), carried to the pass as the white point `203/80 * slider`. The round trip divides and then
   multiplies by the same number, so a wrong white changes what the model is shown, never where
   unchanged pixels land. The `DISPLAYCONFIG_SDR_WHITE_LEVEL` query of the earlier revision is demoted to
   an open question (section 4.3).
3. **The encode binds the original frame as a reference.** A pixel the pass left alone comes back as the
   game's own code, bit for bit (HUD, letterbox, strength zero). Everything else is re-encoded from the
   signed linear result. This is NeuRotic's `PreserveSignedOriginal` idea, written by us.
4. **Two new compute passes (decode, encode) in one new shader file**, not a change to
   `dlssnr.hlsl` and not a change to upstream's `format_transfer`. The pass cbuffer and its HLSL
   struct are untouched, so DEVELOPMENT.md rule 5 is not triggered; the new file has its own constants
   struct and its own check.
5. **Default off.** `[DlssNr] HdrPresent=false`, so DEVELOPMENT.md rule 1 holds: with the key off, the
   decline texts and the frame the game shows are exactly today's. The default flips only after a
   measured run on real HDR (section 8).

D3D12 present host and the D3D11 bridge host get it. Vulkan does not, until the Vulkan present host
exists (section 5.3).

## 2. What exists, and what the earlier revision got wrong

### 2.1 The refusal and who asks

- `PresentColourRefusal(format, colourSpace)` (`DlssNr_PresentColour.h:26`) answers per format first,
  then per colour space: FP16 declines always (`:30-32`); the 8-bit formats accept (`:34-40`); then
  the three Rec.709 gamma spaces (full 2.2, studio 2.2, studio 2.4) accept (`:48-51`), PQ and its YCbCr
  variants decline with a PQ text (`:53-58`), `G10_P709` declines as scRGB (`:60-62`), anything else
  declines as "not sRGB" (`:64-66`).
- D3D12 present hook: `retoque/shaders/dlssnr/DlssNr_Dx12.cpp:6347-6376`. The colour space is
  `ForceHDR && UseHDR10 ? PQ : State::Instance().outputColorSpace.dxgiColorSpace` (`:6360-6362`), the
  format is `GetDesc1()` of the swapchain (`:6364-6366`). A refusal sets `g_resetOnReturn`, says why once
  (`ReportSkipOnce`) and returns before anything is built.
- D3D11 bridge host: `retoque/dlssnr/DlssNr_PresentHost.cpp:347-352`, with `_colourSpace`
  (`DlssNr_PresentHost.h:130`) fed by `NoteColourSpace` from
  `Dx11wDx12SC::SetColorSpace1` (`OptiScaler/with_dx12/dx11_with_dx12_sc.cpp:1507-1521`), **only in the
  branch where `_fgSwapChain != nullptr`**; the other branch (`:1520`) forwards without telling the host.
- The decline cases are pinned by `tests/nr-present-host/cases.h:199-268` (PQ, scRGB, HLG, Rec.2020,
  studio PQ declined; FP16 declined; 8-bit with PQ noted still runs; ten bits with no colour space is
  sRGB and runs). They stay true with `HdrPresent=false`.

### 2.2 What the pass assumes of a present frame

- `PresentFrameDefaults` (`DlssNr_Dx12.cpp:7569-7600`) says every present frame is display-referred:
  `ColourIsLinearHdr = false`, no exposure texture, `PreExposure = 1`. Then
  `isHdrBuffer = frame.ColourIsLinearHdr && FormatCanHoldLinearHdr(desc.Format)` (`:3937-3938`; the format
  test is `:2231-2243`, FP16/FP32/R11G11B10 only) and `Passthrough = isHdrBuffer ? 0 : 1`
  (`:4318`, `:5018`). An SDR present frame is therefore a passthrough frame: the encode is a copy, the
  resolve adds the edit at full scale.
- A present frame that is HDR and linear would take the other branch, the one after-upscale frames
  take: `dlssnr.hlsl:744-790` divides by `WhitePoint()`, rolls the result off with the soft knee or the
  Neutwo/Hybrid proxy, sRGB-encodes it for the model, and keeps the frame value for value in `gKeep`
  (`:753`). The resolve normalises by `normScale = WhitePoint()` and gives back the negative part with
  `signedRest = min(original, 0)` (`:869-875`, `:1256`). `gHighlightRecovery` is "linear HDR only"
  (`:1226`), so it starts to mean something on present frames once they are linear.
- The white point for a linear frame is `DlssNr::Exposure::WhitePoint` (`DlssNr_Exposure.h:30-41`): the
  slider (`DlssNrWhitePointScale`, labelled "Paper white") unless source 1 has a usable game exposure or
  source 2 has an anchor. A present frame has no exposure texture, so source 1 falls back to the slider,
  but a stale `g_nr.gameExposure` from an earlier route would still be accepted by `ValidSample`. That is
  why section 4.2 forces source 0.
- The D3D12 present route copies the backbuffer into `g_bbCopy` in the **backbuffer's own format**
  (`EnsureBbCopy`, `:6206-6218`; copy `:6610-6612`), runs the pass in place on it
  (`EvaluateAtPresent`, `:6654`), and copies it back (`:6692-6696`). `CreateScratch` (`:1893-1918`) gives
  it `ALLOW_UNORDERED_ACCESS`; the typed store into R10G10B10A2 is therefore already exercised by SDR
  ten-bit swapchains, and has not been checked against `D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE`
  anywhere in our code.
- The bridge host already runs the pass on a **FP16 working colour**: `kWorkingFormat =
  R16G16B16A16_FLOAT` (`DlssNr_PresentHost.cpp:22`), `_toWorking` / `_fromWorking` are `FT_Dx12`
  format-transfer objects (`:133-134`) that convert the game's frame in and the result out. Those are
  upstream's passes and are a plain format conversion; they do not know PQ.

### 2.3 How the colour space is known, and where that is weak

DXGI has no `GetColorSpace1`. We learn it from calls we see:

- `WrappedIDXGISwapChain4::SetColorSpace1` (`OptiScaler/wrapped/wrapped_swapchain.cpp:1278-1325`) calls
  the real one, and on success `UpdateOutputColorSpace` (`:61-174`) stores a classification in
  `State::outputColorSpace` (`State.h:147-157`, `:414`). That state is **global**, not per swapchain,
  and is **not reset** on `ResizeBuffers`.
- OptiScaler's own `ForceHDR` calls `_real3->SetColorSpace1` directly in its resize paths
  (`wrapped_swapchain.cpp:1060-1095`, `:1515-1548`), bypassing the hook above. `DlssNr_Dx12.cpp:6356-6362`
  works around it for PQ only (reads the config); a forced scRGB swapchain is invisible to the pass and
  falls through to "format answers first" (FP16 declines).
- The Vulkan side logs `imageColorSpace` at swapchain creation (`OptiScaler/hooks/Vulkan_Hooks.cpp:592-625`)
  in a function-static `lastSpace` and stores nothing.
- `DXGI_COLOR_SPACE_*` for an unset FP16 swapchain is scRGB by default (`DlssNr_PresentColour.h:18-24`
  says so; NeuRotic's `HdrObservation::DefaultColorSpace` makes the same choice).

### 2.4 Corrections to the 2026-09 revision

- Its line references had moved (`DlssNr_Dx12.cpp:5748-5765` is now `:6347-6376`;
  `DlssNr_PresentHost.cpp:115-132` is now `:347-352`).
- `resolveParams.ColourTransform = 1` does not exist. The switch is `frame.ColourIsLinearHdr`
  (`DlssNr_Common.h:101`).
- "Vulkan Present Host: mirror the sandwich" has nothing to mirror: that host is a design note
  ([nr-vulkan-present-host.md](nr-vulkan-present-host.md)), not code, and the Vulkan hook keeps no
  colour state.
- "AP1/ACEScg clamp or soft compression in the encode step" is not how the pass treats out-of-gamut
  values. It keeps them (`signedRest`) and scales saturated results toward the neutral axis
  (`dlssnr.hlsl`, the block above `SanitizeFinite`); no AP1 clamp is added here.
- It put a new HDR pass under "retoque/shaders/dlssnr" and also, implicitly through `FT_Dx12`, upstream's
  `OptiScaler/shaders/format_transfer/`. CLAUDE.md's layout rule says upstream's paths stay byte-identical;
  NeuRotic put its variant there (`shaders/format_transfer/PresentColor_*`), we do not.

## 3. Prior art, and what can and cannot be learned

All four are read-only references. NeuRotic is GPL-3.0 like us, but this note and the code after it are our
own; what follows is cited as prior art.

- **NeuRotic** (MagicalPrincessUnicorn, `~/.cache/neurotic-review/neurotic`; review in
  `~/.local/state/dlss5/notes/neurotic-20261013/report.md` sections 3.3 and 4).
  - `849ed5a9` (0.9.6): `dlssnr/HdrObservation.h`, a per-swapchain registry keyed by COM identity:
    `Register` with the creation format, `RecordColorSpace(requested, HRESULT)`, `BeginResize` /
    `CompleteResize` marking the chain `transitioning`, generation counters so a reused pointer cannot
    inherit state, and `DefaultColorSpace(format)` (FP16 is scRGB, everything else sRGB).
  - `002cc83c` (0.9.7): `dlssnr/PresentColor.h`, `PresentColor::Select(snapshot, actualFormat)`. It returns
    `Unsupported` if the observation is missing, transitioning, or its recorded format differs from the
    swapchain's actual one. It accepts three profiles: SDR709 (R8G8B8A8, B8G8R8A8, **and** R10G10B10A2 under
    `G22_P709`); HDR10 PQ2020 only on R10G10B10A2; scRGB only on FP16. Working format FP16, `whitePoint =
    2.5` (200 nits) hard-coded, `preserveSignedOriginal = hdr`. `FT_Dx12::Transfer::Pq2020ToScRgb` and
    `ScRgbToPq2020` do the conversion; the original PQ frame is bound as a reference at encode.
  - **Their HDR shaders are committed as bytecode only** (`precompile/PresentColor_Shader.h`,
    `PresentColor_Shader_Vk.h`; no `.hlsl` in the tree). Only the contract above can be learned. What we
    take: the three-profile table, the actual-format-must-match rule, the transitioning state, the
    reference-at-encode idea. What we do not take: the 200-nit constant as the only choice, the
    placement under `format_transfer`, the one-commit drop it arrived in.
- **RenoDX** (MIT, `Licenses/RenoDX_ATTRIBUTION.txt`), as recorded in the earlier revision and not
  re-read for this one: `present_path.hpp` queries the display's SDR white level and falls back to 203
  nits; `codec_gain.hpp` treats PQ as absolute (10,000 nits) and scRGB as 80 nits = 1.0; the NR
  integration decodes PQ BT.2020 to linear BT.709 before the model and reconstructs PQ after.
- **DLSS5-Feeder** (MIT), as recorded earlier and not re-read: a verified D3D12 compute pass for ST 2084 to
  linear and back, 203-nit default paper white, round trip within 0.06 of a 10-bit code. That figure is
  its measurement, not ours; section 8 measures our own.
- **wilsjo2** (GPL-3.0), as recorded earlier: `ColourIsLinearHdr = true` and a white point of `203/80` for
  display-referred HDR on its present route, which is the same choice as decision 2 here.
- NeuralScreen (PolyForm Strict): concept only, no code read.

## 4. The conversion chain

### 4.1 Profiles and how each is detected

The decision is a pure function of three inputs the host already has or can cheaply have, **in this
order of authority**: the swapchain's actual format (from `GetDesc1`, as at `DlssNr_Dx12.cpp:6364`), the
colour space last set successfully on **this** swapchain, and `HdrPresent`.

| Swapchain format | Colour space | Profile | Detection | Today |
|---|---|---|---|---|
| R10G10B10A2_UNORM | `RGB_FULL_G2084_NONE_P2020` | **HDR10** (convert) | `SetColorSpace1` on the wrapped swapchain (`wrapped_swapchain.cpp:1278`), or `ForceHDR && UseHDR10` (`:1060-1095`) | declined |
| R16G16B16A16_FLOAT | `RGB_FULL_G10_NONE_P709`, or none ever set | **scRGB** (convert) | the same; FP16 default is scRGB (`DlssNr_PresentColour.h:18-24`) | declined |
| R10G10B10A2_UNORM | `RGB_FULL_G22_NONE_P709`, or none | SDR (unchanged) | default | runs |
| R8G8B8A8, B8G8R8A8, B8G8R8X8 (and `_SRGB`) | any recorded | SDR (unchanged), format decides | DXGI refuses an HDR space on these, so a PQ noted on one is not believed (`cases.h:249`) | runs |
| R10G10B10A2_UNORM | studio PQ, YCbCr PQ, `G22_P2020`, HLG, custom, unknown | **declined** | any of those enums | declined |
| R16G16B16A16_FLOAT | anything explicitly set other than `G10_P709` | **declined** (new, conservative) | same | declined |
| any | observation missing, resize in flight, or recorded format differs from the actual one | **declined** for that frame | see below | not distinguished |

A declined frame logs its reason once, keyed on the literal, as it does now; the literal set grows by
one text per new reason, and the existing texts stay for `HdrPresent=false`.

What has to change in our code to make "the colour space last set on this swapchain" true:

- Record per swapchain, not in the global: a small `{format, colourSpace, resizeGeneration, valid}` held by
  the wrapped swapchain object itself (the object is already per swapchain), written from `SetColorSpace1`
  and from the two `ForceHDR` sites that today bypass it. `State::outputColorSpace` stays as the menu's
  global (`menu_common.cpp:1251`).
- On `ResizeBuffers`/`ResizeBuffers1` with a different format, mark the record not valid until the next
  successful `SetColorSpace1` **or**, if none comes, fall back to the format's default. Whether DXGI keeps
  a colour space across `ResizeBuffers` is not known to this author and is not measured (open question 3);
  declining until the record is re-established is the safe answer either way, and it is NeuRotic's
  `transitioning` rule.
- Tell the bridge host in both branches of `Dx11wDx12SC::SetColorSpace1` (`dx11_with_dx12_sc.cpp:1507-1521`).
- The decision uses the **actual** format of the buffer being copied (`bbDesc.Format`,
  `DlssNr_Dx12.cpp:6422`; the opened D3D11 resource's desc in the bridge); a recorded format that
  differs declines, because a stale colour space applied to the wrong format is the failure this whole
  function exists to prevent.

The decision function lives in `DlssNr_PresentColour.h`, still depending only on `<dxgi1_4.h>` (the
`nr-present-host` suite asserts that include set, `tests/nr-present-host/run.py:41`). Shape:

```
enum class PresentColour { Sdr, Hdr10, ScRgb, Declined };
struct PresentColourDecision { PresentColour profile; const char* reason; /* nullptr unless Declined */ };
PresentColourDecision ResolvePresentColour(DXGI_FORMAT actual, DXGI_FORMAT recordedFormat, bool recordValid,
                                           DXGI_COLOR_SPACE_TYPE colourSpace, bool hdrPresent);
inline const char* PresentColourRefusal(...)  // kept: Declined ? reason : nullptr, with hdrPresent=false
```

`PresentColourRefusal` stays as a wrapper so the existing call sites and cases keep their meaning until the
hosts switch to the new function.

### 4.2 The chain

```
backbuffer (PRESENT) --CopyResource--> g_bbCopy        original codes, backbuffer format, read-only from here
                                           |
                  decode (compute) --------+--> g_hdrWork    linear Rec.709, FP16, 1.0 = 80 nits
                                                    |
                          existing pass, in place (ColourIsLinearHdr = true, white point = 203/80 * slider)
                                                    |
                  encode (compute): g_hdrWork (processed) + g_bbCopy (reference) --> g_hdrOut   backbuffer format
                                                    |
                          CopyResource(backbuffer, g_hdrOut)  --> PRESENT
```

**Decode.**

- HDR10: `code` (float from the UNORM view, k/1023 exactly) to ST 2084 EOTF to nits
  (`Y = 10000 * (max(E^(1/m2) - c1, 0) / (c2 - c3 E^(1/m2)))^(1/m1)`, with m1 = 2610/16384,
  m2 = 2523/4096 * 128, c1 = 3424/4096, c2 = 2413/4096 * 32, c3 = 2392/4096 * 32), then `/80` into scRGB
  units, then the Rec.2020 to Rec.709 matrix. **The result may be negative**: Rec.2020 colours outside
  Rec.709 have negative 709 coordinates. They are kept, not clamped. The matrices are derived from the
  primaries in the host test and compared with the HLSL's literals (section 7); they are not retyped.
- scRGB: the identity; the decode pass is a copy into the FP16 work texture. (It is not skipped, see
  4.4.)
- NaN or Inf in a decoded texel is replaced by zero before it reaches the model; the encode never writes
  a non-finite result (below).

**Run the pass.** Hosts call `PresentFrameDefaults(reset, /*linearHdr=*/true)` (a new defaulted argument;
the SDR call sites are unchanged), and `frame` gains a `PresentWhite` field (`DlssNrFrameInfo`, a C++
struct, not the cbuffer). When `PresentWhite > 0`, the single place that resolves the white point
(`ResolveWhitePoint`, `DlssNr_Dx12.cpp:1863-1873`) returns `PresentWhite * slider` with source 0, no
exposure texture, no scan anchor. So the user's "Paper white" slider multiplies the display white, and
"one quantity, one control" holds: no second nits control exists.

**Encode.** For each texel, in float32, with `dec` = the decode of the original code and `p` = the
processed FP16 texel:

1. If any channel of `p` is not finite: write the original code.
2. If every channel satisfies `|p_c - dec_c| <= eps * max(|dec_c|, floor)`: write the original code.
   Starting values `eps = 2^-9`, `floor = 2^-14` (FP16's smallest normal). Rationale: FP16 carries 11
   significant bits, so decode, the pass's divide-then-multiply by the white point, and the encode
   together move an untouched value by a few ulps; four ulps of slack bypasses that without hiding an
   edit a 10-bit code could show. These two numbers are not tuned; section 7 measures the FP16 round
   trip and the PQ code step and sets them from data.
3. Otherwise HDR10: Rec.709 to Rec.2020 matrix on the signed value, clamp each channel to `[0, 125]`
   scRGB units (10,000 nits), `*80`, ST 2084 inverse EOTF, store (UNORM rounds). scRGB: store `p`
   unchanged, negatives included (FP16 holds them; the swapchain's compositor decides what to do with
   them).
4. Alpha is copied from the original in every branch.

What this does and does not guarantee:

- Untouched pixels, including every HUD pixel the pass's UI correction restored, and every pixel at
  strength zero or with `ApplyModel` off, **come back as the game's own code**. This is the property the
  reference buys and the one the earlier revision's "round trip within 0.06 of a code" could not.
  It is a consequence of rule 2 above and is exact only if the UNORM load and store are an exact
  round trip for k/1023, which this note assumes and the wine suite checks.
- Edited pixels are re-encoded from FP16, so their precision is bounded by FP16, not by 10 bits.
  Banding in a smooth gradient the model touched is possible; whether it shows is a question for the
  in-game run.
- Negative scRGB and out-of-gamut values survive through two mechanisms working together: the pass's own
  `signedRest` (the model never sees them, the composition hands them back), and the reference bypass for
  pixels it did not change.

### 4.3 Paper white

Three choices exist in the prior art: RenoDX queries `DISPLAYCONFIG_SDR_WHITE_LEVEL` and falls back to
203 nits; Feeder and wilsjo2 use 203 nits; NeuRotic uses 200 nits (2.5 scRGB units) unconditionally. The
earlier revision of this note proposed the query.

**Proposal: a fixed 203 nits, scaled by the existing slider; no query in v1.** Reasons, all from the
design above and from what is known about the data:

- The white only decides what the model is shown (`frame / WhitePoint()` before the knee and sRGB encode),
  and the resolve multiplies by the same number (`dlssnr.hlsl:869-875`, `:1256`). The round trip is
  independent of it. A 2x error costs model quality (an image that is darker or brighter than the one it
  was trained on), not colour fidelity.
- The query reports the display's *Windows* SDR content brightness slider. Most HDR games carry their
  own paper-white setting and ignore it; the swapchain does not say what the game chose. The query is
  therefore a prior with no guarantee, on every platform.
- Under Wine/Proton the query may be absent or a stub. That is an assumption of the earlier revision,
  not a measurement; it can be probed with a ten-line program under this machine's prefix, and the
  result is open question 2.
- It removes a second source of truth for the same quantity (DEVELOPMENT.md rule 3).

If the maintainer wants the query anyway: it belongs in the host that resolves `PresentWhite`
(`nits = SDRWhiteLevel / 1000 * 80`, valid range 80 to 480 or fall back to 203), read once per
swapchain and on `WM_DISPLAYCHANGE`, never per frame, and surfaced in the menu readout as "from Windows"
or "default" so a user knows which.

### 4.4 Why decode and encode are separate passes even for scRGB

An in-place run on `g_bbCopy` would save two dispatches and a texture for scRGB, but the encode needs the
untouched original to decide which pixels to leave alone, and the pass writes `g_bbCopy` in place. One
extra FP16 texture and two small dispatches buy the reference for both profiles with one code path. At
3440x1440 `g_hdrWork` is 39.6 MB (FP16) and `g_hdrOut` 19.8 MB (R10G10B10A2) or 39.6 MB (FP16); at 3840x2160
66 MB and 33 or 66 MB. Those are arithmetic, not measurements. The existing present path already owns
several swapchain-sized scratch resources (`g_bbCopy`, the encode's `colorCopy` and `hdrCopy`, the
stabilizer's history); GPU memory on the 3060's 12 GB, and on the 4090 here, is not the limiting factor,
but the tester should look at it.

## 5. Where it plugs in

### 5.1 D3D12 present host (`RunPresentPass`, `DlssNr_Dx12.cpp`)

- `:6347-6376`: replace the refusal with `ResolvePresentColour`. `Declined` keeps today's behaviour
  (reset owed, `ReportSkipOnce`, `ReportRuntimeStatus`, return). `Sdr` falls through unchanged. `Hdr10` and
  `ScRgb` continue with a profile.
- `:6431`: `EnsureBbCopy` as now, plus `EnsureHdrScratch(device, width, height, bbDesc.Format)` for
  `g_hdrWork` (FP16) and `g_hdrOut` (backbuffer format), recreated and the old ones parked with
  `ParkNrResource` on any size or format change, released with `g_bbCopy` at shutdown (`:8591`). Only
  allocated when an HDR profile is decided; an SDR game never allocates them (rule 1).
- `:6610-6612`: unchanged, `g_bbCopy` is still the copy of the backbuffer. It becomes read-only.
- Between that and `EvaluateAtPresent` (`:6654`): decode `g_bbCopy` into `g_hdrWork`; call
  `EvaluateAtPresent` on `g_hdrWork` instead of `g_bbCopy`. The synthesized-motion estimator reads
  `g_bbCopy` today (`:6639`) and should keep reading the original codes (it is a motion estimator on
  luma-like structure; feeding it PQ codes is the same as feeding it SDR codes), not the linear
  texture. That choice is not measured and is open question 5.
- `:6692-6696`: encode `g_hdrWork` + `g_bbCopy` into `g_hdrOut`, then `CopyResource(backbuffer, g_hdrOut)`
  instead of from `g_bbCopy`.
- Submission and completion tracking (`SubmitPresentList`, `:6146-6160`, the per-slot fence) are unchanged;
  the two dispatches are on the same list.
- Hold frame and the comparison modes work unchanged: they act inside the pass on `g_hdrWork`.

### 5.2 D3D11 bridge host (`DlssNr_PresentHost.cpp`)

The bridge's structure already is the sandwich: `_toWorking` (FP16) in, the pass in place, `_fromWorking`
out (`:133-134`, `Record` at `:305`), and `Dx11wDx12SC` copies `Output()` to the presenter
(`dx11_with_dx12_sc.cpp:2024-2032`). With an HDR profile the host replaces the two `FT_Dx12`
dispatches with our decode and encode (the encode's reference is the opened D3D11 backbuffer, already in
`COPY_SOURCE` for `Record`, which needs one more transition to SRV and back). `FT_Dx12` is upstream's and is not edited.
Unknowns specific to the bridge, both unmeasured: whether the D3D11-to-D3D12 shared opening of an FP16 or
R10G10B10A2 flip backbuffer works under Proton at all, and whether the presenter swapchain it creates
carries the same format and receives the colour space (the `SetColorSpace1` forwarding at `:1511` suggests it
is meant to).

### 5.3 Vulkan

Out of scope until there is a Vulkan present host. The native Vulkan route (`EvaluateAfterUpscaleVk`) is
unaffected: it works on the upscaler's output, whose HDR-ness the game states in its DLSS create flags, not
on the swapchain. For the future host ([nr-vulkan-present-host.md](nr-vulkan-present-host.md), "Zero guides
and colour", which says "explicit SDR metadata selects passthrough despite the float format"):

- Detection is at `vkCreateSwapchainKHR` (`Vulkan_Hooks.cpp:592-625` already sees `imageFormat` and
  `imageColorSpace`): `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT` with `R16G16B16A16_SFLOAT` is scRGB;
  `VK_COLOR_SPACE_HDR10_ST2084_EXT` with `A2B10G10R10_UNORM_PACK32` (R10G10B10A2 memory order) is HDR10;
  `A2R10G10B10_UNORM_PACK32` is the B-first order and needs the swizzle; `VK_COLOR_SPACE_HDR10_HLG_EXT`
  and anything else declines. The state is per `VkSwapchainKHR`, created and destroyed with it
  (`hkvkDestroySwapchainKHR`, `:84`, `:182`), not the function-static the logging uses now.
- The shader file gets a SPIR-V target (`-D VK_MODE`) then, not before: shipping a Vulkan header with no
  consumer is dead bytecode that `nr-invariants` would have to guard byte for byte. The HLSL is written
  with the `VK_MODE` binding macros from the start, as `dlssnr.hlsl:1-7` is, so the second target is a build
  step then, not a rewrite. DEVELOPMENT.md rule 8 (drain before free, abandon on a lost device) applies to
  the Vulkan scratch images.

### 5.4 Resources, descriptors, formats (the Proton rule)

Proton is not a D3D12 validator (CLAUDE.md; `13f10d60`). Everything below is checked against D3D12's rules,
not against what runs here, and each item is a review line for the first implementation:

- Constant buffers: the new constants struct is `alignas(256)` with the view size equal to its aligned
  size, as the stabilizer's is (`DlssNr_Stabilizer_Dx12.h`, `struct alignas(256) Constants`). A 48-byte CBV
  ran under vkd3d-proton and killed a 3060.
- Views: SRV of `g_bbCopy` in its real format (R10G10B10A2_UNORM or R16G16B16A16_FLOAT), never a
  reinterpreting format; UAV of `g_hdrWork` as R16G16B16A16_FLOAT; UAV of `g_hdrOut` in the backbuffer's
  format. A swapchain buffer is typed, so no typeless cast is needed. If a game ever hands a TYPELESS
  swapchain format the profile is declined (not in the table above).
- Typed UAV store into R10G10B10A2_UNORM: queried at creation with
  `CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT)` for `D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE`; absent
  means the profile is declined once with its own reason, not a failed dispatch. This is assumed to be
  present on current hardware and unverified on the 3060.
- `CopyResource(backbuffer, g_hdrOut)`: same width, height, format, one mip, one slice, same sample count;
  checked at creation exactly as the existing copies are, and a mismatch declines.
- Barriers: the sequence is a state machine written out in the pass class and reviewed line by line, the
  way the stabilizer's "both rest in NON_PIXEL_SHADER_RESOURCE between dispatches" is. UAV to SRV between
  decode, pass and encode; `g_bbCopy` is `COPY_DEST` then SRV then `COPY_SOURCE`.
- Dispatch dimensions `ceil(w/16) x ceil(h/16)`, as `FT_Dx12`. A descriptor ring of at least the present
  slot count (the stabilizer uses 16 "frames of coverage").

### 5.5 Constants, shader files, config, menu

- **cbuffer rule (DEVELOPMENT.md rule 5).** `dlssnr.hlsl`'s `Params` and `DlssNrConstants` are not touched.
  The new file `retoque/shaders/dlssnr/precompile/dlssnr_present_hdr.hlsl` has its own cbuffer, mirrored by a
  `Constants` struct in a new `DlssNr_PresentHdr_Dx12.{h,cpp}` in the stabilizer's pattern. If a later
  revision needs a field, it appends at the end of both, in the same order.
- **Shader targets (rule 6).** DXIL (`dxc cs_6_0`) into `DlssNr_PresentHdr_Shader.h`; SPIR-V deferred as
  in 5.3. The `.cso` and header are committed together. `nr-invariants` is extended (section 7).
- **Passthrough (rule 7).** The new passes run only on frames that are decided HDR; the existing pass's
  passthrough gate is unchanged, and an SDR frame never reaches them. The encode's own bypass is the only
  new "keep the original" path and carries its own non-finite guard.
- **Config key.** `[DlssNr] HdrPresent`, bool, default `false`. Four points (rule 4): `Config.h`
  `CustomOptional<bool> DlssNrHdrPresent { false }`, read and saved in `Config.cpp`, shipped in
  `OptiScaler.ini`, and an entry in `Config.md`. No nits key, no eps key (constants in the header). The
  exact `PresentColourRefusal` texts stay when it is off.
- **Menu (rule 2, and the DEVELOPMENT.md section 2.1 walk).** A checkbox "HDR output (HDR10 and scRGB)"
  appears in the Colour group **only after the host has seen an HDR swapchain** (a new status accessor next
  to `BufferIsLinearHdr`, `DlssNr_Dx12.cpp:8236`); on an SDR game it does not exist. Below it, one readout
  line: the profile and the white in use (`HDR10, SDR white 203 nits x 1.00`), or the decline reason
  (HLG and others). When the last frame was a converted present frame, the Source combo, Trim and scan rows
  are hidden (nothing drives them), and Paper white stays, labelled as a multiplier of the display white.
  Section 2.1's table gets one row for this state, and the table is walked against the code in review.
- **Status line.** `ReportRuntimeStatus` says the profile once, so a screenshot of the log answers "did it
  convert".
- **Timing.** The two dispatches go inside the GPU-timing scope so the status line's cost includes them
  (`DlssNr_GpuTiming`, one more named interval); an unmeasured change in cost is the thing to avoid.

### 5.6 What else the new input changes

- **Output stabilizer.** It runs on the pass's FP16 output and its creep step is one unit of *its* output
  format (`_quantAbs`, `_quantRel`, `DlssNr_Stabilizer_Dx12.h`). On HDR10 the shown unit is a PQ code, far
  larger than an FP16 ulp; the stabilizer should be built from the *final* format's step, not the work
  texture's. Open question 4.
- **Highlight recovery** (`gHighlightRecovery`, linear HDR only) now applies on present frames; it was
  written for after-upscale frames and its thresholds are relative to the white point
  ([hdr-highlight-recovery.md](hdr-highlight-recovery.md)). Opt-in and unchanged.
- **HUD protection** ([hud-protection.md](hud-protection.md)): the UI correction restores UI pixels from
  the kept frame; with the reference bypass those pixels return as the original codes. Whether the UI
  mask's thresholds behave on a normalised linear frame the way they do on a display-referred one is not
  known; it is part of the in-game run.
- **Frame generation, cadence, periphery:** orthogonal; they act on the pass's raster.

## 6. What stays declined, and fails open

Declined, with a literal reason, game frame untouched: HLG (any), Rec.2020 gamma 2.2, YCbCr colour spaces,
studio-range RGB PQ, custom, unknown enums, FP16 with a non-scRGB space set, a TYPELESS or otherwise
unlisted swapchain format, a swapchain whose record is missing, mid-resize or mismatched, a missing typed-UAV
store, a failed scratch allocation, `HdrPresent=false`. Fails open the same way the pass does now:
`g_nr.failed`, `reportSkip`, the game's own backbuffer is what is presented. No profile guesses.

## 7. Test plan

**Host tier (no GPU).**

- `tests/nr-present-host`: new cases for `ResolvePresentColour`, a table over format x colour space x
  `recordValid` x `recordedFormat == actual` x `hdrPresent`. With `hdrPresent=false` every existing case
  at `cases.h:199-268` must produce the same text it produces now (default-identical, rule 1). With it on,
  HDR10 and scRGB convert; every row of the section 4.1 table that says declined declines; the bridge
  is told the colour space in both branches (a fake `Dx11wDx12SC` call).
- New suite `tests/nr-hdr-math` (registered in `tests/suites.toml`): the reference transfer functions in
  float64 plus an FP16 emulation. Checks: the PQ round trip over all 1024 codes of each channel and over
  a grid of primaries and secondaries; **the encode rule on a ramp**, i.e. for every code, decode, round to
  FP16, apply the section 4.2 bypass test, assert the output code equals the input code (this is where
  `eps` and `floor` are set from data, and where the claim "four ulps of slack" stands or falls); the
  smallest relative PQ code step over 64 to 1023 (to confirm the bypass hides nothing a code could show);
  negative 709 coordinates survive; NaN/Inf inputs; the Rec.2020/709 matrices are inverses within 1e-6;
  and a text comparison that the HLSL's matrix literals and constants equal the header's (the same kind
  of check `nr-invariants` makes for the cbuffer).
- `nr-invariants` impact: check 1 (config round trip) picks up `HdrPresent` and fails until all four points
  exist; check 3 (headers equal bytecode) must learn the new pass, `DlssNr_PresentHdr_Shader.{cso,h}`
  arriving together; a new check 5 asserts the new constants struct equals its cbuffer (rule 5 for the new
  pair). No retired identifiers. `dlssnr.hlsl` and its two headers must remain byte-identical to
  `93730d39`, which is the mechanical proof that decision 4 held.

**Wine tier** (new `tests/nr-hdr-d3d12`, serial, modelled on `tests/synth-motion-d3d12`: the production
pass class compiled with a `pch.h` shim, vkd3d-proton in its own prefix, no NGX, no game).

- Synthetic R10G10B10A2 frames carrying every code 0 to 1023 in a ramp plus the gamut grid, and FP16
  frames with values from -2 to 125 including negatives, 0, subnormals, Inf, NaN.
- Identity: decode, no edit, encode. Assert **every output code equals its input code, bit for bit**, for
  both profiles. This is the test of the reference bypass and of the UNORM round-trip assumption.
- Known edit: a synthetic processed texture that differs from the decoded original by a known amount in
  a region; assert codes outside it are untouched and codes inside are within one code of the independent
  float64 prediction.
- scRGB negatives and values above 1 survive; NaN input leaves the original.
- The CBV size and format-support queries are asserted in the harness, but **vkd3d-proton does not prove
  D3D12 validity**: this suite is evidence that the maths and the dispatch work, not that native Windows
  accepts them. The only validator is native D3D12 with the debug layer, which is what the Windows tester's
  run (below) is for.
- A second, optional leg in `tests/dlssnr-loopback` (`--present-nr`, a separate switch): the same static
  fixture presented as HDR10 through the real model, saved before and after, to look at the picture. It
  needs the model and is not a pass/fail gate.

**In-game candidates.** The test machine is Linux/Proton with a Samsung LC34G55T; whether that display's
HDR mode reaches Wine through this machine's compositor is **not measured here**, and a game only selects
an HDR colour space if `CheckColorSpaceSupport` says yes. First step on this machine, before any code:
look for the existing line `DLSS-NR: swapchain colour space ...` (`wrapped_swapchain.cpp:1278-1325`) in a log from a
game with an HDR toggle. The honest plan is two legs:

1. **Plumbing on this machine without an HDR display.** OptiScaler's own `ForceHDR` (with `UseHDR10` for
   PQ) puts an SDR game into an HDR swapchain format and colour space. The picture is not HDR content
   (SDR values read as 80-nit scRGB are dim), but it exercises profile detection, decode, the pass, and
   the exact-return property against the same game's SDR frame. Candidates: Divinity: Original Sin 2
   (D3D11 bridge), The Witcher 3 DirectX 11 (bridge), PCSX2 (D3D12 present pass, `~/Games/pcsx2-nr-test/run.sh`).
   Whether ForceHDR holds under Proton for these is itself unmeasured.
2. **Real HDR content on the Windows RTX 3060 tester** with an HDR display and the native D3D12 debug layer:
   a title that presents R10G10B10A2 PQ or FP16 scRGB through D3D12 (Cyberpunk 2077, which is installed
   here and has an HDR option, is the first candidate; Monster Hunter Wilds and Baldur's Gate 3 are the titles
   NeuRotic's notes report working, which is their claim, not a measurement of ours). Whether the tester
   has an HDR display is open question 6.

Visual checks, each with a reference: sun discs and specular highlights do not clip or tint; dark caves
show no colour cast; HUD pixels are bit-identical to the HdrPresent-off frame (a capture of both; see
[comparison-capture.md](comparison-capture.md), which is what makes this a measurement); a scRGB title's
out-of-gamut colours do not shift hue.

**What "done" means.**

1. The host suites, including the new cases and `nr-hdr-math`, and `nr-invariants` pass; `dlssnr.hlsl` and its
   headers are unchanged.
2. The wine suite passes the identity test bit for bit for both profiles, and the known-edit test within one
   code.
3. DEVELOPMENT.md section 3 is walked adversarially for the config key, the menu (section 2.1 table updated
   and walked row by row) and the new shader.
4. One real HDR10 title and one scRGB title (or ForceHDR as a stand-in for the second, stated as such) run
   with `HdrPresent=true` and show the conversion in the log, no cast, no clipped highlights, HUD
   identical; the cost delta is read from the GPU timing, not estimated.
5. The tester's run on native D3D12 with the debug layer completes with no device removal and no debug
   layer errors from the new passes. Until then the key stays default off and the README says HDR is
   experimental.
6. `README.md`'s "declined" text and the status line are updated in the same change as the code.

## 8. Risks

1. **Wrong colour space believed.** The record is learned from calls, not queried. A stale or missing record
   would decode PQ as sRGB or the reverse, and the composition would write that over the frame. Mitigated by
   the per-swapchain record, the actual-format rule, the resize rule, and fail-open on any mismatch; not
   mitigated against a game that sets a colour space and does not render in it (ForceHDR does exactly
   this by design).
2. **Out-of-gamut colour.** Rec.2020 values outside Rec.709 are negative in the working space. They are
   preserved (`signedRest`, the bypass), but the model is shown the non-negative part, and a saturated
   result is scaled toward neutral rather than clipped per channel. Measured only on after-upscale frames so
   far.
3. **Precision.** FP16 for edited pixels. Banding in a gradient the model touched is possible; the bypass
   protects the pixels it did not touch. Unmeasured on our side.
4. **Typed UAV store on R10G10B10A2.** Required by design, assumed present, queried at creation.
5. **Cost.** Two extra passes and one copy per frame; the model's cost dominates but the number is not
   known. It is read from the GPU timing in the run.
6. **Proton hides D3D12 errors.** Section 5.4 and the tester's run exist for this.
7. **Downstream compositors.** What a compositor does with a scRGB negative, with paper white above its
   own, or with an HDR10 frame whose metadata the game set for a different peak, is outside our code;
   we do not call `SetHDRMetaData`.
8. **The bridge.** Whether it supports an HDR D3D11 swapchain at all is unmeasured.

## 9. Open questions that need the maintainer

1. **Default for `HdrPresent`**: stay off until a measured run on real HDR (as proposed), or on after the
   wine suite alone?
2. **Query SDR white or not.** Proposed: not in v1. Needs, if wanted, a probe of
   `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL` under this prefix and on the tester's machine.
3. **Does DXGI keep the colour space across `ResizeBuffers`?** Declining until re-established is safe but
   may cost frames; a short probe under native D3D12 would settle it.
4. **Stabilizer step on HDR**: build it from the final format's quantum (proposed), or leave it on the work
   texture's?
5. **Which texture the synthesized-motion estimator reads** on an HDR frame, original codes (proposed) or
   the linear work texture.
6. **Does the Windows tester have an HDR display**, and which HDR title do they have installed?
7. **SPIR-V for the new file now or with the Vulkan host** (proposed: with the Vulkan host).
8. **Should the Windows `SDR white` slider be visible** for users who want to tell us their game's paper
   white? Proposed: no, the Paper white multiplier is enough, and a second control for the same quantity
   breaks rule 3.
