# HDR10 and scRGB Present Conversion: Decode to Linear BT.709 and Re-encode

Status: **design note; not yet implemented.** Replaces the refusal in `DlssNr_PresentColour.h` with an in-place decode and re-encode around the present-time pass.

---

## 1. Where this comes from

The refusal in `dlssnr/DlssNr_PresentColour.h:8-16,26-58` explicitly deferred HDR presentation:
> *"until the hosts convert PQ and scRGB into what the model was trained on and back again, such a frame is declined"*.

The mathematical solution and architecture are established across community injectors:

1. **RenoDX** (`v8.5dev`, commit `9bb6c0f`, MIT licence, `Licenses/RenoDX_ATTRIBUTION.txt`):
   - `present_path.hpp:1194-1248`: queries display SDR reference white using `DISPLAYCONFIG_SDR_WHITE_LEVEL` (`nits = SDRWhiteLevel / 1000.f * 80.f`) when running in HDR, falling back to `NRDiffuseWhiteNits` (203 nits).
   - `codec_gain.hpp:32-36, 62`: maps PQ as absolute (10,000 nits max) and scRGB as 80 nits = 1.0.
   - `dlssnr.hpp:5553-5557` and `shaders/v6_commit.cs_5_1.hlsl:55-57`: converts PQ BT.2020 to linear BT.709 before NR evaluation, and reconstructs BT.2020 PQ on writeback.
2. **DLSS5-Feeder** (commit `7b41f5f`, MIT licence):
   - `src/feed_pq12.h`: verified D3D12 compute pass converting ST 2084 PQ to linear and back. Benchmarks demonstrated that a PQ -> linear -> FP16 -> PQ round trip lands within 0.06 of a 10-bit code value.
   - `src/dlss5-feed.cpp:1008-1024`: defaults `hdr_paper_white` to 203 nits (ITU-R BT.2408 reference).
3. **wilsjo2** (`OptiScaler-DLSSNR-PreSR-Multipass`, commit `e237f895`, GPL-3.0 licence):
   - `shaders/dlssnr/DlssNr_Late.inl:324-326, 439-444`: sets `ColourIsLinearHdr = true` and `WhitePointOverride = 203.0f / 80.0f` for display-referred HDR.
   - `precompile/dlssnr_finished_color.hlsl`: implements mode 0 (`mul(to709, DecodePQ(pixel.rgb))`) and mode 1 (`EncodePQ(mul(to2020, pixel.rgb))`) with finite fallback.
4. **NeuralScreen** (`docs/HDR.md`):
   - Evaluated for conceptual contract only (`proxy = sRGB(max(C,0)/(W+P))`); source is under PolyForm Strict licence, so no code is used.

---

## 2. What exists in this codebase today

- **Refusal table:** `OptiScaler/dlssnr/DlssNr_PresentColour.h:35-58` intercepts present calls and rejects:
  - `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020` (HDR10 PQ);
  - `DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709` (scRGB);
  - Formats `DXGI_FORMAT_R10G10B10A2_UNORM` and `DXGI_FORMAT_R16G16B16A16_FLOAT` when paired with non-sRGB colour spaces.
- **Present-time call sites:**
  - `OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp:5748-5765` (`RunPresentPass`): queries `PresentColourRefusal` and skips recording on refusal.
  - `OptiScaler/dlssnr/DlssNr_PresentHost.cpp:115-132` (D3D11 bridge host): queries `PresentColourRefusal` and skips bridge capture.
  - `OptiScaler/dlssnr/DlssNrFeature_Vk.cpp` / Vulkan present path: declines non-sRGB swapchains.
- **Signed-value pipeline:**
  - `OptiScaler/dlssnr/README.md` ("Signed values survive the pass"): commit `75e62d22` ensures linear HDR frames with negative chromaticity values (arising from wide gamut conversion into BT.709) preserve negative channels instead of clamping them to zero.

---

## 3. Proposed design

Instead of aborting presentation on HDR swapchains, wrap the present-time DLSS-NR pass in a pre-decode and post-encode stage:

```
[ Swapchain Backbuffer (HDR10 PQ / scRGB) ]
                    │
                    ▼
       ┌─────────────────────────┐
       │   Stage 1: HDR Decode   │
       │  - ST 2084 PQ or scRGB  │
       │  - Normalize by W (nits)│
       │  - BT.2020 -> BT.709    │
       └─────────────────────────┘
                    │  (Linear scene-referred BT.709 in FP16)
                    ▼
       ┌─────────────────────────┐
       │   DLSS-NR Pipeline      │
       │  - Encode / Model / Res │
       │  - Signed values kept   │
       │  - Paper white = 1.0    │
       └─────────────────────────┘
                    │  (Enhanced Linear BT.709)
                    ▼
       ┌─────────────────────────┐
       │   Stage 2: HDR Encode   │
       │  - BT.709 -> BT.2020    │
       │  - Multiply by W (nits) │
       │  - Linear -> PQ / scRGB │
       └─────────────────────────┘
                    │
                    ▼
[ Enhanced Backbuffer to Swapchain Present ]
```

### 3.1 Mathematical formulation

1. **SDR Reference White ($W$):**
   - Query `DisplayConfigGetDeviceInfo` with `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL`.
   - If successful and running in HDR: $W = \frac{\text{SDRWhiteLevel}}{1000} \times 80.0 \text{ nits}$.
   - If unqueried, invalid or zero: fallback to $W = 203.0 \text{ nits}$ (ITU-R BT.2408 recommendation).
2. **PQ Decode (HDR10):**
   - Input: $E' \in [0, 1]$ in $R10G10B10A2$ or $R16G16B16A16$.
   - $Y = \left(\frac{\max(E'^{1/m_2} - c_1, 0)}{c_2 - c_3 E'^{1/m_2}}\right)^{1/m_1} \times 10,000 \text{ nits}$.
   - Normalized linear: $C_{\text{2020}} = \frac{Y}{W}$.
   - Gamut mapping to BT.709: $C_{\text{709}} = M_{\text{2020}\to\text{709}} \times C_{\text{2020}}$.
3. **scRGB Decode:**
   - Input: $C_{\text{scRGB}} \in (-\infty, +\infty)$ in $R16G16B16A16\_FLOAT$, where $1.0 = 80 \text{ nits}$.
   - Normalized linear: $C_{\text{709}} = C_{\text{scRGB}} \times \frac{80.0}{W}$.
4. **Re-encode:**
   - Inverse matrix: $C_{\text{2020}} = M_{\text{709}\to\text{2020}} \times C_{\text{709}}$.
   - Scale to absolute nits: $Y = C_{\text{2020}} \times W$.
   - Apply inverse EOTF (ST 2084) for PQ, or scale by $\frac{W}{80.0}$ for scRGB.
   - Fallback protection: if re-encoded pixel contains NaN or Inf, return original backbuffer texel.

---

## 4. Integration points

1. **`OptiScaler/dlssnr/DlssNr_PresentColour.h`:**
   - Restrict `PresentColourRefusal` to unsupported formats (e.g. HLG `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` or unknown enum values).
   - Export helper `ResolveSdrWhiteNits(bool forceHdr, float userOverride)`.
2. **`OptiScaler/shaders/dlssnr/`:**
   - Add compute shader pass `dlssnr_hdr_present.hlsl` (compiled to DXIL and SPIR-V headers):
     - Pass 0: Decode (PQ/scRGB -> Linear BT.709 FP16);
     - Pass 1: Encode (Linear BT.709 FP16 -> PQ/scRGB).
3. **`OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp` (`RunPresentPass`):**
   - Bind intermediate FP16 target for decoded frame.
   - Configure `resolveParams.ColourTransform = 1` (Linear HDR).
   - Set paper white constant to $1.0$ (since normalization by $W$ already occurs in decode).
4. **`OptiScaler/dlssnr/DlssNr_PresentHost.cpp` and Vulkan Present Host:**
   - Mirror the decode/encode sandwich around the bridge swapchain blit.

---

## 5. Interactions and conflicts

- **Signed values:** Mandatory cooperation with commit `75e62d22`. Wide-gamut BT.2020 colors when transformed to BT.709 produce valid negative component values. Clamping to zero before NR alters highlight chrominance and causes green/magenta shifts.
- **Output Stabilizer:** Runs in linear space; stabilizer thresholds (`StabilizerTolerance`) must scale relative to normalized $W$.
- **Periphery Compression & Model Cadence:** Orthogonal; both operate on the intermediate model raster after decode.

---

## 6. Risks

1. **Proton / Wine query support:**
   - Under Proton/Wine, `DisplayConfigGetDeviceInfo` frequently stubs or returns failure for display capability queries.
   - *Mitigation:* The 203-nit reference fallback must be unconditional on query failure.
2. **Out-of-gamut color clipping:**
   - Saturated colors in BT.2020 can exceed BT.709 gamut boundaries, producing negative values or values $> 1.0$.
   - *Mitigation:* AP1 / ACEScg clamp or soft compression in the encode step prevents numerical explosion in the model.
3. **Precision loss:**
   - Round trip through FP16 scratch buffer could introduce 10-bit banding in smooth gradients.
   - *Mitigation:* Feeder demonstrated maximum error $< 0.06$ of 1 code value on 10-bit HDR10; FP16 mantissa provides 11 bits of precision in $[0.5, 1.0]$.

---

## 7. Test plan

1. **Host tier unit test (`tests/nr-hdr-convert`):**
   - Synthetic mathematical verification: round-trip test covering 0.001 nits to 10,000 nits across primary and secondary colors.
   - Assert max deviation $\le 1$ code value in 10-bit PQ.
   - Verify negative values in BT.709 survive intact through round trip.
2. **In-game validation:**
   - Title 1 (D3D12 HDR10 PQ): Cyberpunk 2077 or The Witcher 3 (Next-Gen DX12) with HDR enabled.
   - Title 2 (D3D11 bridge HDR): The Witcher 3 (DX11 mode with SpecialK/Windows 11 AutoHDR or native HDR backbuffer).
   - Visual checks: no color cast in dark caves, no clipping of sun discs or specular highlights.

---

## 8. Open questions

- Does the gamescope/Wayland HDR layer on Linux communicate calibrated display white point to DXVK/Proton?
- Should the user be provided with a manual "HDR Paper White (Nits)" slider in the menu to tune reference diffuse white?
