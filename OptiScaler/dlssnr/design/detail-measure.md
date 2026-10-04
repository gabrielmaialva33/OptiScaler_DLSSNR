# Measure Detail: In-game Repeatable Image Metric

Status: **implemented on D3D12 behind "Measure detail" button in Compare menu; off by default; allocates nothing until pressed.** Ported from janblade's OptiScaler fork (GPL-3.0).

---

## 1. Where this comes from

Originates from **janblade** (`OptiScaler` fork, `origin/main` branch):
- **Commits:**
  - `daeb388f` (2026-09-30): introduced the "Measure detail" button, compute pass, and readback ring;
  - `28daeebf` (2026-09-30): added OkLab chromaticity / warmth metrics and shadow crushing / darkening statistics.
- **Shader:**
  `OptiScaler/shaders/dlssnr/precompile/dlssnr_detail_stats.hlsl`:
  A 64x64 tile compute pass measuring 1/4 of pixels across the frame into a 256x64 grid of float4 values.
- **Licence:** GPL-3.0 (same licence as this repository).

---

## 2. What exists in this codebase today

- **Existing measurement instruments:**
  - **Side-by-side / Wipe Compare (`DlssNr_Dx12.cpp:5138-5185`):** Visual qualitative comparison for the human eye, but provides no numerical metrics.
  - **Frame Capture (`DlssNr_Capture.h`):** Writes matched uncompressed PNG dumps of pre-NR and post-NR frames to disk for external offline inspection.
  - **GPU Timing (`DlssNr_GpuTiming.cpp`):** Measures queue and kernel execution times via fence-confirmed timestamp queries; measures execution duration only, saying nothing about image fidelity, detail or artifacts.
- **The gap:**
  Parameters such as `Cadence`, `StabilizerStrength`, `Transfer=2` (DLSS private SR), `PeripheryCompression` and `Transfer=3` (Catmull-Rom) sit in documentation as "not yet measured" for visual impact. Evaluating their effect previously depended on subjective impressions or video recordings.

---

## 3. Proposed design

"Measure detail" operates on a still camera scene (ideally with `HoldFrame` or in photo/pause mode) across 60 evaluations (~1 second at 60 fps):

1. **Metrics measured per evaluation:**
   - **Band detail ($B_1 - B_2$):** Difference of two box-binomial blurs ($\sigma \approx 1.2 \text{ px}$ vs $\sigma \approx 3.0 \text{ px}$). Rejects single-pixel high-frequency grain while capturing structural sharpness at the model's scale.
   - **Raw detail:** 4-neighbour Laplacian $|\Delta L|$ (sensitive to raw edge contrast and single-pixel grain).
   - **Flicker:** Mean absolute frame-to-frame change of the output minus the input's own temporal change. Measures temporal instability introduced by the model beyond native scene motion.
   - **Colour shift & Saturation:** Evaluated in OkLab space ($L, a, b$) at the frozen white point. Measures relative chroma change and warmth shift ($b^*_{\text{out}} - b^*_{\text{in}}$).
   - **Shadows:** Evaluated where input display-encoded luma is $< 0.15$ (~2% of white). Measures percentage of shadow darkening, shadow lifting, and crushed pixels ($< 50\%$ of input level).
2. **Comparison against previous run:**
   - Retains the immediately preceding measurement to output relative diffs:
     `vs previous: detail +12.3%, flicker -0.00120, saturation -1.5%`.

---

## 4. What this metric DOES NOT prove

- **It is NOT a general perceptual quality score:**
  A high "detail added" reading does not guarantee a visually superior image; excessive ringing, sharpening halos, or synthetic hallucinations will register as positive detail.
- **It is NOT an in-motion metric:**
  Designed exclusively for still scenes. If the camera or geometry moves, motion parallax registers as high flicker and corrupted detail deltas.
- **It is NOT an absolute benchmark across different scenes:**
  Measurements are relative within the same framing. Comparing numbers across different scenes or lighting conditions is meaningless.

---

## 5. Implementation details

- **Architecture:**
  - `OptiScaler/dlssnr/DlssNr_DetailStats.h`: pure CPU header containing `ReduceGrid()` and `Measurement` aggregation.
  - `OptiScaler/shaders/dlssnr/DlssNr_DetailStats_Dx12.h/.cpp`: D3D12 compute pass running `dlssnr_detail_stats.hlsl`.
  - `OptiScaler/dlssnr/DlssNr_Menu.cpp`: button and readout under Compare section.
- **Resource allocation:**
  - Zero allocation at startup. Scratch textures and readback ring are instantiated only when the user clicks "Measure detail".
  - Resources retire safely through `Park*` lists upon completion or cancellation.

---

## 6. Test plan

- **Host unit test (`tests/nr-detail-measure`):**
  - Verify `ReduceGrid` against synthetic grid data (uniform values, known outliers, NaN/Inf handling).
  - Verify `Measurement` statistics (flicker, saturation, shadow crushing calculations).
  - Verify textual output and comparison deltas.
- **In-game test:**
  - Freeze scene with `HoldFrame`. Click "Measure detail" with `Transfer=1`.
  - Switch to `Transfer=3` (sharp residual). Measure again; verify that `detail added` increases and `Compare` reports the delta accurately.
