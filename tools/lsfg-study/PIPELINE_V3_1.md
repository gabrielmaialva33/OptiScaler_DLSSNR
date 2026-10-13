# LSFG 3.1 Pass Graph Specification (lsfg-vk 2.0.0 on D3D12)

## 1. Overview and Architecture

Lossless Scaling Frame Generation (LSFG 3.1) is an optical-flow based frame interpolation model executing entirely within compute shaders. It receives two consecutive input frames $I_{t-1}$ and $I_t$ (provided as a 2-layer 2D texture array) and synthesizes intermediate frame(s) at normalized timestamp $t \in (0, 1)$.

### High-Level Pass Breakdown
1. **Pre-Pass (Command List 0: Stages 0..9, 34 passes):** Executed **once per input frame pair**.
   - `mipmaps` (Stage 0): Downsamples current source frame into a 7-level luminance mip pyramid (R8_UNORM).
   - `alpha0`..`alpha3` (Stages 1..4): Multi-scale feature extraction across all 7 pyramid levels in parallel, producing 12-channel (Quality) or 6-channel (Performance) feature descriptors `alphaArray[0..6]`.
   - `beta0`..`beta4` (Stages 5..9): Motion candidate analysis on coarse scale 0, generating a 6-level motion cost/guidance pyramid `betaImageArray` (R8_UNORM).
2. **Main-Pass (Command List 1: Stages 10..45, 66 passes):** Executed **$(M - 1)$ times** for an $M\times$ multiplier.
   - `gamma0`..`gamma4` (Stages 10..29, 30..44): Coarse-to-fine optical flow field pyramid estimation (7 levels).
   - `delta0`..`delta4` (Stages 10..24): Reverse/bidirectional flow consistency and occlusion mask estimation (levels 4..6).
   - `epsilon0`..`epsilon4` (Stages 30..44): Context-aware flow refinement and blending weight pyramid (levels 4..6).
   - `generate` (Stage 45): Final bidirectional backward warping, occlusion blending, and UI/text preservation at native resolution.

### Command List Splitting
- **Command List 0 (Pre-Pass):** Contains Stages 0..9 (Passes 0..33). Dispatched once per new input frame.
- **Command List 1 (Main-Pass):** Contains Stages 10..45 (Passes 34..99). Dispatched $(M - 1)$ times with updated uniform buffer timestamps.

---

## 2. Image Manifest & Allocation Table

All 101 image resources declared by the pipeline signature. In lsfg-vk, external inputs/outputs have dedicated allocations, while internal images are partitioned into Pinned (Segment 1) and Transient (Segment 0 with aliased interval coloring).

| Img ID | Symbolic Name | Format (SDR / HDR) | Layers | ExtentOp Formula | Extent @ 1920x1080 (F=1.0) | Extent @ 1920x1080 (F=0.85) | Lifetime (Stages) | Flags | Persistent History |
|---|---|---|---|---|---|---|---|---|---|
| 0 | `sourceImageArray` | RGBA8888 / RGBA16161616 | 2 | `Base` | 1920x1080 | 1920x1080 | Stages 0..45 | ExternalInput, Pinned, HdrVariant | **YES (Ping-Pong)** |
| 1 | `mipmapImageArray` | R8 | 7 mips | `Flow` | 1920x1080 | 1632x918 | Stages 0..1 | Mipmaps | No |
| 2 | `alpha0_ff_0` | RGBA8888 | 2 | `(Base >> 6 + 1) >> 1` | 15x8 | 15x8 | Stages 1..2 | Transient | No |
| 3 | `alpha1_ff_0` | RGBA8888 | 2 | `(Base >> 6 + 1) >> 1` | 15x8 | 15x8 | Stages 2..3 | Transient | No |
| 4 | `alpha2_ff_0` | RGBA8888 | 4 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 3..4 | Transient | No |
| 5 | `alpha3_res_0` | RGBA8888 | 12 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 0..45 | Pinned | Retained in Frame |
| 6 | `alpha0_ff_1` | RGBA8888 | 2 | `(Base >> 5 + 1) >> 1` | 30x17 | 30x17 | Stages 1..2 | Transient | No |
| 7 | `alpha1_ff_1` | RGBA8888 | 2 | `(Base >> 5 + 1) >> 1` | 30x17 | 30x17 | Stages 2..3 | Transient | No |
| 8 | `alpha2_ff_1` | RGBA8888 | 4 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 3..4 | Transient | No |
| 9 | `alpha3_res_1` | RGBA8888 | 12 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 0..45 | Pinned | Retained in Frame |
| 10 | `alpha0_ff_2` | RGBA8888 | 2 | `(Base >> 4 + 1) >> 1` | 60x34 | 60x34 | Stages 1..2 | Transient | No |
| 11 | `alpha1_ff_2` | RGBA8888 | 2 | `(Base >> 4 + 1) >> 1` | 60x34 | 60x34 | Stages 2..3 | Transient | No |
| 12 | `alpha2_ff_2` | RGBA8888 | 4 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 3..4 | Transient | No |
| 13 | `alpha3_res_2` | RGBA8888 | 12 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 0..45 | Pinned | Retained in Frame |
| 14 | `alpha0_ff_3` | RGBA8888 | 2 | `(Base >> 3 + 1) >> 1` | 120x68 | 120x68 | Stages 1..2 | Transient | No |
| 15 | `alpha1_ff_3` | RGBA8888 | 2 | `(Base >> 3 + 1) >> 1` | 120x68 | 120x68 | Stages 2..3 | Transient | No |
| 16 | `alpha2_ff_3` | RGBA8888 | 4 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 3..4 | Transient | No |
| 17 | `alpha3_res_3` | RGBA8888 | 12 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 0..45 | Pinned | Retained in Frame |
| 18 | `alpha0_ff_4` | RGBA8888 | 2 | `(Base >> 2 + 1) >> 1` | 240x135 | 240x135 | Stages 1..2 | Transient | No |
| 19 | `alpha1_ff_4` | RGBA8888 | 2 | `(Base >> 2 + 1) >> 1` | 240x135 | 240x135 | Stages 2..3 | Transient | No |
| 20 | `alpha2_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 3..4 | Transient | No |
| 21 | `alpha3_res_4` | RGBA8888 | 12 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 0..45 | Pinned | Retained in Frame |
| 22 | `alpha0_ff_5` | RGBA8888 | 2 | `(Base >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 1..2 | Transient | No |
| 23 | `alpha1_ff_5` | RGBA8888 | 2 | `(Base >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 2..3 | Transient | No |
| 24 | `alpha2_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 3..4 | Transient | No |
| 25 | `alpha3_res_5` | RGBA8888 | 12 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 0..45 | Pinned | Retained in Frame |
| 26 | `alpha0_ff_6` | RGBA8888 | 2 | `(Base >> 0 + 1) >> 1` | 960x540 | 960x540 | Stages 1..2 | Transient | No |
| 27 | `alpha1_ff_6` | RGBA8888 | 2 | `(Base >> 0 + 1) >> 1` | 960x540 | 960x540 | Stages 2..3 | Transient | No |
| 28 | `alpha2_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 3..4 | Transient | No |
| 29 | `alpha3_res_6` | RGBA8888 | 12 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 0..45 | Pinned | Retained in Frame |
| 30 | `beta0_ff` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 5..6 | Transient | No |
| 31 | `beta1_ff` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 6..7 | Transient | No |
| 32 | `beta2_ff` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 7..8 | Transient | No |
| 33 | `beta3_ff` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 8..9 | Transient | No |
| 34 | `betaImageArray` | R8 | 6 mips | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 9..44 | Mipmaps | No |
| 35 | `gamma0_ff_0` | RGBA8888 | 3 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 10..11 | Transient | No |
| 36 | `gamma1_ff_0` | RGBA8888 | 4 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 11..12 | Transient | No |
| 37 | `gamma2_ff_0` | RGBA8888 | 4 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 12..13 | Transient | No |
| 38 | `gamma3_ff_0` | RGBA8888 | 4 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 13..14 | Transient | No |
| 39 | `gamma4_res_0` | RGBA16161616 | 1 | `((Base >> 6 + 1) >> 1 + 1) >> 1` | 8x4 | 8x4 | Stages 14..19 | Transient | No |
| 40 | `gamma0_ff_1` | RGBA8888 | 3 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 15..16 | Transient | No |
| 41 | `gamma1_ff_1` | RGBA8888 | 4 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 16..17 | Transient | No |
| 42 | `gamma2_ff_1` | RGBA8888 | 4 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 17..18 | Transient | No |
| 43 | `gamma3_ff_1` | RGBA8888 | 4 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 18..19 | Transient | No |
| 44 | `gamma4_res_1` | RGBA16161616 | 1 | `((Base >> 5 + 1) >> 1 + 1) >> 1` | 15x9 | 15x9 | Stages 19..24 | Transient | No |
| 45 | `gamma0_ff_2` | RGBA8888 | 3 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 20..21 | Transient | No |
| 46 | `gamma1_ff_2` | RGBA8888 | 4 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 21..22 | Transient | No |
| 47 | `gamma2_ff_2` | RGBA8888 | 4 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 22..23 | Transient | No |
| 48 | `gamma3_ff_2` | RGBA8888 | 4 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 23..24 | Transient | No |
| 49 | `gamma4_res_2` | RGBA16161616 | 1 | `((Base >> 4 + 1) >> 1 + 1) >> 1` | 30x17 | 30x17 | Stages 24..29 | Transient | No |
| 50 | `gamma0_ff_3` | RGBA8888 | 3 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 25..26 | Transient | No |
| 51 | `gamma1_ff_3` | RGBA8888 | 4 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 26..27 | Transient | No |
| 52 | `gamma2_ff_3` | RGBA8888 | 4 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 27..28 | Transient | No |
| 53 | `gamma3_ff_3` | RGBA8888 | 4 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 28..29 | Transient | No |
| 54 | `gamma4_res_3` | RGBA16161616 | 1 | `((Base >> 3 + 1) >> 1 + 1) >> 1` | 60x34 | 60x34 | Stages 29..34 | Transient | No |
| 55 | `gamma0_ff_4` | RGBA8888 | 3 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 30..31 | Transient | No |
| 56 | `gamma1_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 31..32 | Transient | No |
| 57 | `gamma2_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 32..33 | Transient | No |
| 58 | `gamma3_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 33..34 | Transient | No |
| 59 | `gamma4_res_4` | RGBA16161616 | 1 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 34..39 | Transient | No |
| 60 | `delta0_ff_4` | RGBA8888 | 3 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 10..11 | Transient | No |
| 61 | `delta1_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 11..12 | Transient | No |
| 62 | `delta2_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 12..13 | Transient | No |
| 63 | `delta3_ff_4` | RGBA8888 | 4 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 13..14 | Transient | No |
| 64 | `delta4_res_4` | RGBA16161616 | 1 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 14..35 | Transient | No |
| 65 | `epsilon0_ff_4` | RGBA8888 | 2 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 30..31 | Transient | No |
| 66 | `epsilon1_ff_4` | RGBA8888 | 2 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 31..32 | Transient | No |
| 67 | `epsilon2_ff_4` | RGBA8888 | 2 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 32..33 | Transient | No |
| 68 | `epsilon3_ff_4` | RGBA8888 | 2 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 33..34 | Transient | No |
| 69 | `epsilon4_res_4` | RGBA16161616 | 1 | `((Base >> 2 + 1) >> 1 + 1) >> 1` | 120x68 | 120x68 | Stages 34..39 | Transient | No |
| 70 | `gamma0_ff_5` | RGBA8888 | 3 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 35..36 | Transient | No |
| 71 | `gamma1_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 36..37 | Transient | No |
| 72 | `gamma2_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 37..38 | Transient | No |
| 73 | `gamma3_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 38..39 | Transient | No |
| 74 | `gamma4_res_5` | RGBA16161616 | 1 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 39..44 | Transient | No |
| 75 | `delta0_ff_5` | RGBA8888 | 3 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 15..16 | Transient | No |
| 76 | `delta1_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 16..17 | Transient | No |
| 77 | `delta2_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 17..18 | Transient | No |
| 78 | `delta3_ff_5` | RGBA8888 | 4 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 18..19 | Transient | No |
| 79 | `delta4_res_5` | RGBA16161616 | 1 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 19..40 | Transient | No |
| 80 | `epsilon0_ff_5` | RGBA8888 | 2 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 35..36 | Transient | No |
| 81 | `epsilon1_ff_5` | RGBA8888 | 2 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 36..37 | Transient | No |
| 82 | `epsilon2_ff_5` | RGBA8888 | 2 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 37..38 | Transient | No |
| 83 | `epsilon3_ff_5` | RGBA8888 | 2 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 38..39 | Transient | No |
| 84 | `epsilon4_res_5` | RGBA16161616 | 1 | `((Base >> 1 + 1) >> 1 + 1) >> 1` | 240x135 | 240x135 | Stages 39..44 | Transient | No |
| 85 | `gamma0_ff_6` | RGBA8888 | 3 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 40..41 | Transient | No |
| 86 | `gamma1_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 41..42 | Transient | No |
| 87 | `gamma2_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 42..43 | Transient | No |
| 88 | `gamma3_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 43..44 | Transient | No |
| 89 | `gamma4_res_6` | RGBA16161616 | 1 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 44..45 | Transient | No |
| 90 | `delta0_ff_6` | RGBA8888 | 3 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 20..21 | Transient | No |
| 91 | `delta1_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 21..22 | Transient | No |
| 92 | `delta2_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 22..23 | Transient | No |
| 93 | `delta3_ff_6` | RGBA8888 | 4 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 23..24 | Transient | No |
| 94 | `delta4_res_6` | RGBA16161616 | 1 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 24..45 | Transient | No |
| 95 | `epsilon0_ff_6` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 40..41 | Transient | No |
| 96 | `epsilon1_ff_6` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 41..42 | Transient | No |
| 97 | `epsilon2_ff_6` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 42..43 | Transient | No |
| 98 | `epsilon3_ff_6` | RGBA8888 | 2 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 43..44 | Transient | No |
| 99 | `epsilon4_res_6` | RGBA16161616 | 1 | `((Base >> 0 + 1) >> 1 + 1) >> 1` | 480x270 | 480x270 | Stages 44..45 | Transient | No |
| 100 | `generate_res` | RGBA8888 / RGBA16161616 | 1 | `Base` | 1920x1080 | 1920x1080 | Stages 0..45 | ExternalOutput, Pinned, HdrVariant | Retained in Frame |

### Temporal Persistence (History)
- **`sourceImageArray` (Image 0):** This is the **only persistent resource across frames**. It is a 2D Texture Array with 2 layers:
  - Layer `cb_SourceIndex % 2`: holds the newly uploaded current frame $I_t$.
  - Layer `(cb_SourceIndex + 1) % 2`: holds the previous frame $I_{t-1}$ from the prior dispatch.
- **All intermediate feature images (`alphaArray`, `betaImageArray`, `gamma`, `delta`, `epsilon`):** Are purely instantaneous for the current frame pair. They do NOT persist across frame pairs (no recurrent states).
- **`alphaArray[0..6]` (Images 5, 9, 13, 17, 21, 25, 29):** Marked `Pinned` because they are produced once in the Pre-Pass (Cmdbuf 0) and read repeatedly by all subiterations of the Main-Pass (Cmdbuf 1) when multiplier > 2.

---

## 3. Root Signature & Descriptor Table Layout

In the converted HLSL, every Vulkan binding $N$ is mapped to its own D3D12 register space: `register(<class>0, spaceN)`.
The D3D12 Root Signature can be constructed as follows:

1. **Root Parameter 0: ConstantBufferView (CBV)** at `register(b0, space0)` (UniformBuffer).
2. **Root Parameter 1: 32-bit Root Constants** (2 DWORDs) at `register(b0, space0)` (`pc_FirstIter`, `pc_Iteration`).
3. **Root Parameter 2..4: Static Samplers** (or Sampler Descriptor Table):
   - Space 1: Sampler 0 (`s0, space1`)
   - Space 2: Sampler 1 (`s0, space2`)
   - Space 3: Sampler 2 (`s0, space3`)
4. **Root Parameters 5..56: Descriptor Tables for Bindings 4..55** (one table per space $N$):

| Vulkan Binding | Register / Space | Type | Descriptor Count | Bound Pipeline Image Indices & Description |
|---|---|---|---|---|
| 4 | `register(t0, space4)` | SRV | 1 | sourceImageArray (0) |
| 5 | `register(u0, space5)` | UAV | 7 | mipmapImageArray (1) |
| 6 | `register(t0, space6)` | SRV | 7 | mipmapImageArray (1) |
| 7 | `register(u0, space7)` | UAV | 7 | alpha0_ff_0 (2), alpha0_ff_1 (6), alpha0_ff_2 (10), ... (+4 more) |
| 8 | `register(t0, space8)` | SRV | 7 | alpha0_ff_0 (2), alpha0_ff_1 (6), alpha0_ff_2 (10), ... (+4 more) |
| 9 | `register(u0, space9)` | UAV | 7 | alpha1_ff_0 (3), alpha1_ff_1 (7), alpha1_ff_2 (11), ... (+4 more) |
| 10 | `register(t0, space10)` | SRV | 7 | alpha1_ff_0 (3), alpha1_ff_1 (7), alpha1_ff_2 (11), ... (+4 more) |
| 11 | `register(u0, space11)` | UAV | 7 | alpha2_ff_0 (4), alpha2_ff_1 (8), alpha2_ff_2 (12), ... (+4 more) |
| 12 | `register(t0, space12)` | SRV | 7 | alpha2_ff_0 (4), alpha2_ff_1 (8), alpha2_ff_2 (12), ... (+4 more) |
| 13 | `register(u0, space13)` | UAV | 7 | alpha3_res_0 (5), alpha3_res_1 (9), alpha3_res_2 (13), ... (+4 more) |
| 14 | `register(t0, space14)` | SRV | 7 | alpha3_res_0 (5), alpha3_res_1 (9), alpha3_res_2 (13), ... (+4 more) |
| 15 | `register(u0, space15)` | UAV | 1 | beta0_ff (30) |
| 16 | `register(t0, space16)` | SRV | 1 | beta0_ff (30) |
| 17 | `register(u0, space17)` | UAV | 1 | beta1_ff (31) |
| 18 | `register(t0, space18)` | SRV | 1 | beta1_ff (31) |
| 19 | `register(u0, space19)` | UAV | 1 | beta2_ff (32) |
| 20 | `register(t0, space20)` | SRV | 1 | beta2_ff (32) |
| 21 | `register(u0, space21)` | UAV | 1 | beta3_ff (33) |
| 22 | `register(t0, space22)` | SRV | 1 | beta3_ff (33) |
| 23 | `register(u0, space23)` | UAV | 6 | betaImageArray (34) |
| 24 | `register(t0, space24)` | SRV | 6 | betaImageArray (34) |
| 25 | `register(u0, space25)` | UAV | 7 | gamma0_ff_0 (35), gamma0_ff_1 (40), gamma0_ff_2 (45), ... (+4 more) |
| 26 | `register(t0, space26)` | SRV | 7 | gamma0_ff_0 (35), gamma0_ff_1 (40), gamma0_ff_2 (45), ... (+4 more) |
| 27 | `register(u0, space27)` | UAV | 7 | gamma1_ff_0 (36), gamma1_ff_1 (41), gamma1_ff_2 (46), ... (+4 more) |
| 28 | `register(t0, space28)` | SRV | 7 | gamma1_ff_0 (36), gamma1_ff_1 (41), gamma1_ff_2 (46), ... (+4 more) |
| 29 | `register(u0, space29)` | UAV | 7 | gamma2_ff_0 (37), gamma2_ff_1 (42), gamma2_ff_2 (47), ... (+4 more) |
| 30 | `register(t0, space30)` | SRV | 7 | gamma2_ff_0 (37), gamma2_ff_1 (42), gamma2_ff_2 (47), ... (+4 more) |
| 31 | `register(u0, space31)` | UAV | 7 | gamma3_ff_0 (38), gamma3_ff_1 (43), gamma3_ff_2 (48), ... (+4 more) |
| 32 | `register(t0, space32)` | SRV | 7 | gamma3_ff_0 (38), gamma3_ff_1 (43), gamma3_ff_2 (48), ... (+4 more) |
| 33 | `register(u0, space33)` | UAV | 7 | gamma4_res_0 (39), gamma4_res_1 (44), gamma4_res_2 (49), ... (+4 more) |
| 34 | `register(t0, space34)` | SRV | 7 | gamma4_res_0 (39), gamma4_res_1 (44), gamma4_res_2 (49), ... (+4 more) |
| 35 | `register(u0, space35)` | UAV | 3 | delta0_ff_4 (60), delta0_ff_5 (75), delta0_ff_6 (90) |
| 36 | `register(t0, space36)` | SRV | 3 | delta0_ff_4 (60), delta0_ff_5 (75), delta0_ff_6 (90) |
| 37 | `register(u0, space37)` | UAV | 3 | delta1_ff_4 (61), delta1_ff_5 (76), delta1_ff_6 (91) |
| 38 | `register(t0, space38)` | SRV | 3 | delta1_ff_4 (61), delta1_ff_5 (76), delta1_ff_6 (91) |
| 39 | `register(u0, space39)` | UAV | 3 | delta2_ff_4 (62), delta2_ff_5 (77), delta2_ff_6 (92) |
| 40 | `register(t0, space40)` | SRV | 3 | delta2_ff_4 (62), delta2_ff_5 (77), delta2_ff_6 (92) |
| 41 | `register(u0, space41)` | UAV | 3 | delta3_ff_4 (63), delta3_ff_5 (78), delta3_ff_6 (93) |
| 42 | `register(t0, space42)` | SRV | 3 | delta3_ff_4 (63), delta3_ff_5 (78), delta3_ff_6 (93) |
| 43 | `register(u0, space43)` | UAV | 3 | delta4_res_4 (64), delta4_res_5 (79), delta4_res_6 (94) |
| 44 | `register(t0, space44)` | SRV | 3 | delta4_res_4 (64), delta4_res_5 (79), delta4_res_6 (94) |
| 45 | `register(u0, space45)` | UAV | 3 | epsilon0_ff_4 (65), epsilon0_ff_5 (80), epsilon0_ff_6 (95) |
| 46 | `register(t0, space46)` | SRV | 3 | epsilon0_ff_4 (65), epsilon0_ff_5 (80), epsilon0_ff_6 (95) |
| 47 | `register(u0, space47)` | UAV | 3 | epsilon1_ff_4 (66), epsilon1_ff_5 (81), epsilon1_ff_6 (96) |
| 48 | `register(t0, space48)` | SRV | 3 | epsilon1_ff_4 (66), epsilon1_ff_5 (81), epsilon1_ff_6 (96) |
| 49 | `register(u0, space49)` | UAV | 3 | epsilon2_ff_4 (67), epsilon2_ff_5 (82), epsilon2_ff_6 (97) |
| 50 | `register(t0, space50)` | SRV | 3 | epsilon2_ff_4 (67), epsilon2_ff_5 (82), epsilon2_ff_6 (97) |
| 51 | `register(u0, space51)` | UAV | 3 | epsilon3_ff_4 (68), epsilon3_ff_5 (83), epsilon3_ff_6 (98) |
| 52 | `register(t0, space52)` | SRV | 3 | epsilon3_ff_4 (68), epsilon3_ff_5 (83), epsilon3_ff_6 (98) |
| 53 | `register(u0, space53)` | UAV | 3 | epsilon4_res_4 (69), epsilon4_res_5 (84), epsilon4_res_6 (99) |
| 54 | `register(t0, space54)` | SRV | 3 | epsilon4_res_4 (69), epsilon4_res_5 (84), epsilon4_res_6 (99) |
| 55 | `register(u0, space55)` | UAV | 1 | generate_res (100) |

---

## 4. Sampler Configurations

All three samplers use bilinear filtering without mipmapping bias:

```cpp
// Sampler 0: Bilinear ClampToBorder (Transparent Black 0,0,0,0)
D3D12_SAMPLER_DESC sampler0 = {};
sampler0.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
sampler0.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler0.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler0.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler0.BorderColor[0..3] = 0.0f; // Transparent Black
sampler0.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
sampler0.MinLOD = 0.0f;
sampler0.MaxLOD = D3D12_FLOAT32_MAX;

// Sampler 1: Bilinear ClampToBorder (Opaque White 1,1,1,1)
D3D12_SAMPLER_DESC sampler1 = {};
sampler1.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
sampler1.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler1.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler1.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
sampler1.BorderColor[0..3] = 1.0f; // Opaque White
sampler1.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
sampler1.MinLOD = 0.0f;
sampler1.MaxLOD = D3D12_FLOAT32_MAX;

// Sampler 2: Bilinear ClampToEdge
D3D12_SAMPLER_DESC sampler2 = {};
sampler2.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
sampler2.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
sampler2.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
sampler2.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
sampler2.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
sampler2.MinLOD = 0.0f;
sampler2.MaxLOD = D3D12_FLOAT32_MAX;
```

---

## 5. Constant Buffers & Push Constants

### Uniform Buffer Layout (`register(b0, space0)`)

```hlsl
cbuffer type_cb : register(b0, space0)
{
    float cb_Timestamp;           // c0.x: Interpolation factor in (0.0, 1.0)
    uint  cb_SourceIndex;         // c0.y: Frame index counter (ping-pong % 2)
    uint  cb_AdvancedColorKind;   // c0.z: Color transform: 0=standard, 1=gamma/pow(0.4545), 2=HDR
    uint  cb_HdrSupport;          // c0.w: HDR enabled: 0=SDR, 1=HDR
    float cb_ResolutionInvScale;  // c1.x: 1.0f / flow (e.g. 1.0 at 100% flow, 1.17647 at 85% flow)
    float cb_UiThreshold;         // c1.y: Default 0.5f
};
```

### Push Constants / 32-bit Root Constants

```hlsl
cbuffer type_PushConstant_PC : register(b0, space0)
{
    uint pc_FirstIter;   // c0.x: 1 if first pyramid iteration (i == 0 or i == 4), 0 otherwise
    uint pc_Iteration;   // c0.y: Subiteration index within the shader's execution sequence
};
```

### Per-Frame and Multi-Multiplier Runtime Logic

Let $M$ be the Frame Generation Multiplier (e.g. $M = 2, 3, 4$):
1. When a new game frame arrives:
   - Ping-pong layer selection: upload new frame into `sourceImageArray` layer `cb_SourceIndex % 2`.
   - Execute **Command List 0 (Pre-Pass)**: builds `mipmaps`, `alphaArray[0..6]`, and `betaImageArray`.
2. For each generated frame $j \in [0, M - 2]$:
   - Set normalized timestamp: $$cb\_Timestamp = \frac{j + 1}{M}$$
     - $M = 2$: $j = 0 \implies t = 0.5$ (1 intermediate frame exactly halfway).
     - $M = 3$: $j = 0 \implies t = 0.3333$, $j = 1 \implies t = 0.6667$.
     - $M = 4$: $j = 0 \implies t = 0.25$, $j = 1 \implies t = 0.5$, $j = 2 \implies t = 0.75$.
   - Execute **Command List 1 (Main-Pass)**: produces intermediate frame in `generate_res`.
   - Copy `generate_res` to swapchain/target before dispatching next $j$.
3. After all generated frames for this cycle are presented:
   - Present current real frame $I_t$.
   - Increment `cb_SourceIndex++` so the next incoming frame flips layers.

---

## 6. Complete Pass Graph & Execution Order

Below is the complete, strictly-ordered sequence of all 46 execution stages and 100 passes.
For each shader, the Resource ID (RID) is shown for all four variants: Quality FP32 (Q32), Quality FP16 (Q16), Performance FP32 (P32), Performance FP16 (P16).

| Stage | Cmdbuf | Pass | Shader | RID (Q32/Q16/P32/P16) | Threads | pc_FirstIter | pc_Iteration | Dispatch Extent Formula | Dispatch @ 1920x1080 (F=1.0) | Input Bindings & Resources | Output Bindings & Resources |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 0 | 0 | `mipmaps` | 0/0/0/0 | 32x32 | 0 | 0 | `(Base + 63) >> 6` | (30, 17, 1) | `sourceImageArray` | `mipmapImageArray` |
| 1 | 0 | 1 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 0 | `((Base >> 6 + 1) >> 1 + 7) >> 3` | (2, 1, 1) | `mipmapImageArray`[layer 6] | `alpha0_ff_0` |
| 1 | 0 | 2 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 1 | `((Base >> 5 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `mipmapImageArray`[layer 5] | `alpha0_ff_1` |
| 1 | 0 | 3 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 2 | `((Base >> 4 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `mipmapImageArray`[layer 4] | `alpha0_ff_2` |
| 1 | 0 | 4 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 3 | `((Base >> 3 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `mipmapImageArray`[layer 3] | `alpha0_ff_3` |
| 1 | 0 | 5 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 4 | `((Base >> 2 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `mipmapImageArray`[layer 2] | `alpha0_ff_4` |
| 1 | 0 | 6 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 5 | `((Base >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `mipmapImageArray`[layer 1] | `alpha0_ff_5` |
| 1 | 0 | 7 | `alpha0` | 13/61/37/85 | 8x8 | 0 | 6 | `((Base >> 0 + 1) >> 1 + 7) >> 3` | (120, 68, 1) | `mipmapImageArray`[layer 0] | `alpha0_ff_6` |
| 2 | 0 | 8 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 0 | `((Base >> 6 + 1) >> 1 + 7) >> 3` | (2, 1, 1) | `alpha0_ff_0` | `alpha1_ff_0` |
| 2 | 0 | 9 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 1 | `((Base >> 5 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `alpha0_ff_1` | `alpha1_ff_1` |
| 2 | 0 | 10 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 2 | `((Base >> 4 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `alpha0_ff_2` | `alpha1_ff_2` |
| 2 | 0 | 11 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 3 | `((Base >> 3 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha0_ff_3` | `alpha1_ff_3` |
| 2 | 0 | 12 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 4 | `((Base >> 2 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha0_ff_4` | `alpha1_ff_4` |
| 2 | 0 | 13 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 5 | `((Base >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha0_ff_5` | `alpha1_ff_5` |
| 2 | 0 | 14 | `alpha1` | 14/62/38/86 | 8x8 | 0 | 6 | `((Base >> 0 + 1) >> 1 + 7) >> 3` | (120, 68, 1) | `alpha0_ff_6` | `alpha1_ff_6` |
| 3 | 0 | 15 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `alpha1_ff_0` | `alpha2_ff_0` |
| 3 | 0 | 16 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `alpha1_ff_1` | `alpha2_ff_1` |
| 3 | 0 | 17 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `alpha1_ff_2` | `alpha2_ff_2` |
| 3 | 0 | 18 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `alpha1_ff_3` | `alpha2_ff_3` |
| 3 | 0 | 19 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha1_ff_4` | `alpha2_ff_4` |
| 3 | 0 | 20 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha1_ff_5` | `alpha2_ff_5` |
| 3 | 0 | 21 | `alpha2` | 15/63/39/87 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha1_ff_6` | `alpha2_ff_6` |
| 4 | 0 | 22 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `alpha2_ff_0` | `alpha3_res_0` |
| 4 | 0 | 23 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `alpha2_ff_1` | `alpha3_res_1` |
| 4 | 0 | 24 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `alpha2_ff_2` | `alpha3_res_2` |
| 4 | 0 | 25 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `alpha2_ff_3` | `alpha3_res_3` |
| 4 | 0 | 26 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha2_ff_4` | `alpha3_res_4` |
| 4 | 0 | 27 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha2_ff_5` | `alpha3_res_5` |
| 4 | 0 | 28 | `alpha3` | 16/64/40/88 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha2_ff_6` | `alpha3_res_6` |
| 5 | 0 | 29 | `beta0` | 22/70/46/94 | 8x8 | 0 | 0 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha3_res_6` | `beta0_ff` |
| 6 | 0 | 30 | `beta1` | 23/71/47/95 | 8x8 | 0 | 0 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `beta0_ff` | `beta1_ff` |
| 7 | 0 | 31 | `beta2` | 24/72/48/96 | 8x8 | 0 | 0 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `beta1_ff` | `beta2_ff` |
| 8 | 0 | 32 | `beta3` | 25/73/49/97 | 8x8 | 0 | 0 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `beta2_ff` | `beta3_ff` |
| 9 | 0 | 33 | `beta4` | 26/74/50/98 | 32x32 | 0 | 0 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 31) >> 5` | (15, 9, 1) | `beta3_ff` | `betaImageArray` |
| 10 | 0 | 34 | `delta0` | 8/56/32/80 | 8x8 | 1 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha3_res_4`, `INVALID` | `delta0_ff_4` |
| 10 | 0 | 35 | `gamma0` | 3/51/27/75 | 8x8 | 1 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `alpha3_res_0`, `INVALID` | `gamma0_ff_0` |
| 11 | 0 | 36 | `delta1` | 9/57/33/81 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `delta0_ff_4` | `delta1_ff_4` |
| 11 | 0 | 37 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `gamma0_ff_0` | `gamma1_ff_0` |
| 12 | 0 | 38 | `delta2` | 10/58/34/82 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `delta1_ff_4` | `delta2_ff_4` |
| 12 | 0 | 39 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `gamma1_ff_0` | `gamma2_ff_0` |
| 13 | 0 | 40 | `delta3` | 11/59/35/83 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `delta2_ff_4` | `delta3_ff_4` |
| 13 | 0 | 41 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `gamma2_ff_0` | `gamma3_ff_0` |
| 14 | 0 | 42 | `delta4` | 12/60/36/84 | 8x8 | 1 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `delta3_ff_4`, `INVALID`, `betaImageArray`[layer 2] | `delta4_res_4` |
| 14 | 0 | 43 | `gamma4` | 7/55/31/79 | 8x8 | 1 | 0 | `(((Base >> 6 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (1, 1, 1) | `gamma3_ff_0`, `INVALID`, `betaImageArray`[layer 5] | `gamma4_res_0` |
| 15 | 0 | 44 | `delta0` | 8/56/32/80 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha3_res_5`, `delta4_res_4` | `delta0_ff_5` |
| 15 | 0 | 45 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `alpha3_res_1`, `gamma4_res_0` | `gamma0_ff_1` |
| 16 | 0 | 46 | `delta1` | 9/57/33/81 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `delta0_ff_5` | `delta1_ff_5` |
| 16 | 0 | 47 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `gamma0_ff_1` | `gamma1_ff_1` |
| 17 | 0 | 48 | `delta2` | 10/58/34/82 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `delta1_ff_5` | `delta2_ff_5` |
| 17 | 0 | 49 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `gamma1_ff_1` | `gamma2_ff_1` |
| 18 | 0 | 50 | `delta3` | 11/59/35/83 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `delta2_ff_5` | `delta3_ff_5` |
| 18 | 0 | 51 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `gamma2_ff_1` | `gamma3_ff_1` |
| 19 | 0 | 52 | `delta4` | 12/60/36/84 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `delta3_ff_5`, `delta4_res_4`, `betaImageArray`[layer 1] | `delta4_res_5` |
| 19 | 0 | 53 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 1 | `(((Base >> 5 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (2, 2, 1) | `gamma3_ff_1`, `gamma4_res_0`, `betaImageArray`[layer 5] | `gamma4_res_1` |
| 20 | 0 | 54 | `delta0` | 8/56/32/80 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha3_res_6`, `delta4_res_5` | `delta0_ff_6` |
| 20 | 0 | 55 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `alpha3_res_2`, `gamma4_res_1` | `gamma0_ff_2` |
| 21 | 0 | 56 | `delta1` | 9/57/33/81 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `delta0_ff_6` | `delta1_ff_6` |
| 21 | 0 | 57 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `gamma0_ff_2` | `gamma1_ff_2` |
| 22 | 0 | 58 | `delta2` | 10/58/34/82 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `delta1_ff_6` | `delta2_ff_6` |
| 22 | 0 | 59 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `gamma1_ff_2` | `gamma2_ff_2` |
| 23 | 0 | 60 | `delta3` | 11/59/35/83 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `delta2_ff_6` | `delta3_ff_6` |
| 23 | 0 | 61 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `gamma2_ff_2` | `gamma3_ff_2` |
| 24 | 0 | 62 | `delta4` | 12/60/36/84 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `delta3_ff_6`, `delta4_res_5`, `betaImageArray`[layer 0] | `delta4_res_6` |
| 24 | 0 | 63 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 2 | `(((Base >> 4 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (4, 3, 1) | `gamma3_ff_2`, `gamma4_res_1`, `betaImageArray`[layer 4] | `gamma4_res_2` |
| 25 | 0 | 64 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `alpha3_res_3`, `gamma4_res_2` | `gamma0_ff_3` |
| 26 | 0 | 65 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `gamma0_ff_3` | `gamma1_ff_3` |
| 27 | 0 | 66 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `gamma1_ff_3` | `gamma2_ff_3` |
| 28 | 0 | 67 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `gamma2_ff_3` | `gamma3_ff_3` |
| 29 | 0 | 68 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 3 | `(((Base >> 3 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (8, 5, 1) | `gamma3_ff_3`, `gamma4_res_2`, `betaImageArray`[layer 3] | `gamma4_res_3` |
| 30 | 0 | 69 | `epsilon0` | 17/65/41/89 | 8x8 | 1 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha3_res_4`, `gamma4_res_3`, `INVALID` | `epsilon0_ff_4` |
| 30 | 0 | 70 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `alpha3_res_4`, `gamma4_res_3` | `gamma0_ff_4` |
| 31 | 0 | 71 | `epsilon1` | 18/66/42/90 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `epsilon0_ff_4` | `epsilon1_ff_4` |
| 31 | 0 | 72 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `gamma0_ff_4` | `gamma1_ff_4` |
| 32 | 0 | 73 | `epsilon2` | 19/67/43/91 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `epsilon1_ff_4` | `epsilon2_ff_4` |
| 32 | 0 | 74 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `gamma1_ff_4` | `gamma2_ff_4` |
| 33 | 0 | 75 | `epsilon3` | 20/68/44/92 | 8x8 | 0 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `epsilon2_ff_4` | `epsilon3_ff_4` |
| 33 | 0 | 76 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `gamma2_ff_4` | `gamma3_ff_4` |
| 34 | 1 | 77 | `epsilon4` | 21/69/45/93 | 8x8 | 1 | 0 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `epsilon3_ff_4`, `INVALID` | `epsilon4_res_4` |
| 34 | 1 | 78 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 4 | `(((Base >> 2 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (15, 9, 1) | `gamma3_ff_4`, `gamma4_res_3`, `betaImageArray`[layer 2] | `gamma4_res_4` |
| 35 | 1 | 79 | `epsilon0` | 17/65/41/89 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha3_res_5`, `gamma4_res_4`, `delta4_res_4` | `epsilon0_ff_5` |
| 35 | 1 | 80 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `alpha3_res_5`, `gamma4_res_4` | `gamma0_ff_5` |
| 36 | 1 | 81 | `epsilon1` | 18/66/42/90 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `epsilon0_ff_5` | `epsilon1_ff_5` |
| 36 | 1 | 82 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `gamma0_ff_5` | `gamma1_ff_5` |
| 37 | 1 | 83 | `epsilon2` | 19/67/43/91 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `epsilon1_ff_5` | `epsilon2_ff_5` |
| 37 | 1 | 84 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `gamma1_ff_5` | `gamma2_ff_5` |
| 38 | 1 | 85 | `epsilon3` | 20/68/44/92 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `epsilon2_ff_5` | `epsilon3_ff_5` |
| 38 | 1 | 86 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `gamma2_ff_5` | `gamma3_ff_5` |
| 39 | 1 | 87 | `epsilon4` | 21/69/45/93 | 8x8 | 0 | 1 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `epsilon3_ff_5`, `epsilon4_res_4` | `epsilon4_res_5` |
| 39 | 1 | 88 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 5 | `(((Base >> 1 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (30, 17, 1) | `gamma3_ff_5`, `gamma4_res_4`, `betaImageArray`[layer 1] | `gamma4_res_5` |
| 40 | 1 | 89 | `epsilon0` | 17/65/41/89 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha3_res_6`, `gamma4_res_5`, `delta4_res_5` | `epsilon0_ff_6` |
| 40 | 1 | 90 | `gamma0` | 3/51/27/75 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `alpha3_res_6`, `gamma4_res_5` | `gamma0_ff_6` |
| 41 | 1 | 91 | `epsilon1` | 18/66/42/90 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `epsilon0_ff_6` | `epsilon1_ff_6` |
| 41 | 1 | 92 | `gamma1` | 4/52/28/76 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `gamma0_ff_6` | `gamma1_ff_6` |
| 42 | 1 | 93 | `epsilon2` | 19/67/43/91 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `epsilon1_ff_6` | `epsilon2_ff_6` |
| 42 | 1 | 94 | `gamma2` | 5/53/29/77 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `gamma1_ff_6` | `gamma2_ff_6` |
| 43 | 1 | 95 | `epsilon3` | 20/68/44/92 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `epsilon2_ff_6` | `epsilon3_ff_6` |
| 43 | 1 | 96 | `gamma3` | 6/54/30/78 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `gamma2_ff_6` | `gamma3_ff_6` |
| 44 | 1 | 97 | `epsilon4` | 21/69/45/93 | 8x8 | 0 | 2 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `epsilon3_ff_6`, `epsilon4_res_5` | `epsilon4_res_6` |
| 44 | 1 | 98 | `gamma4` | 7/55/31/79 | 8x8 | 0 | 6 | `(((Base >> 0 + 1) >> 1 + 1) >> 1 + 7) >> 3` | (60, 34, 1) | `gamma3_ff_6`, `gamma4_res_5`, `betaImageArray`[layer 0] | `gamma4_res_6` |
| 45 | 1 | 99 | `generate` | 1/2/1/2 | 16x16 | 0 | 0 | `(Base + 15) >> 4` | (120, 68, 1) | `sourceImageArray`, `gamma4_res_6`, `delta4_res_6`, `epsilon4_res_6` | `generate_res` |

---

## 7. Synchronization & Barriers

### In-Stage Concurrency
- Passes within the same stage (e.g. 7 `alpha0` invocations in Stage 1, or `delta` + `gamma` in Stage 10) operate on **disjoint subresources** (different array slices or different textures).
- In D3D12, all dispatches inside a single stage can execute concurrently without intra-stage barriers.

### Inter-Stage Barriers
- At the boundary between Stage $K$ and Stage $K+1$:
  - Any resource written as UAV in Stage $K$ that is read as SRV in Stage $K+1$ must have a state transition or UAV barrier:
    ```cpp
    D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::UAV(pResource);
    // or transition from UNORDERED_ACCESS to NON_PIXEL_SHADER_RESOURCE
    ```
  - At the end of Command List 0 (Stage 9), all pinned feature images (`alphaArray[0..6]`, `betaImageArray`) must transition to `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE` before Command List 1 begins.

---

## 8. Verification & Reference Frames

### How `lsfg-vk-cli debug` Consumes Images
Analysis of `lsfg-vk-cli/src/debug.cpp` reveals how input images are consumed:
1. **Input Format:** Standard 128-byte uncompressed DDS header (`DDPF_RGB | DDPF_ALPHAPIXELS`, 32bpp RGBA8 UNORM), followed by `width * height * 4` raw pixel bytes.
2. **Ordering:** Files in the input folder are sorted by the integer prefix before the first period (e.g. `0.dds`, `1.dds`, `2.dds`).
3. **Upload Mapping:** Frame $i$ is copied via host image copy (`vkCopyMemoryToImageEXT`) into `sourceImageArray` layer `i % 2`.
4. **Frame 0 vs Frame 1:** At $i = 0$, only layer 0 is initialized. At $i = 1$, layer 1 is loaded, and the pipeline executes full interpolation between $I_0$ and $I_1$.

### Ground Truth Generation Command
To produce the ground truth reference frames in `Tmp/lsfg-study/ref/`, run:

```bash
# Generate translation and rotation/zoom reference frames:
python3 tools/lsfg-study/make_test_frames.py --run-cli

# Or directly invoke lsfg-vk-cli debug:
lsfg-vk-cli debug -w 1920 -h 1080 -m 2 -f 1.0 -o Tmp/lsfg-study/ref/translation Tmp/lsfg-study/input_translation
lsfg-vk-cli debug -w 1920 -h 1080 -m 2 -f 1.0 -o Tmp/lsfg-study/ref/rotzoom Tmp/lsfg-study/input_rotzoom
```

### Comparing D3D12 Output against Vulkan Reference

Use `tools/lsfg-study/compare.py` to compare any candidate D3D12 frame with the reference ground truth:

```bash
python3 tools/lsfg-study/compare.py Tmp/lsfg-study/ref/translation/1.dds d3d12_translation.dds
python3 tools/lsfg-study/compare.py Tmp/lsfg-study/ref/rotzoom/1.dds d3d12_rotzoom.dds --threshold 2.0
```
