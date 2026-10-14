# LSFG 3.1 FP16 Investigation Notes

**Status (2026-10-14): FP16 stays off; section 7 has the measurements.** A native 16-bit conversion (SM 6.2,
`-enable-16bit-types`) of ids 51..98 was built and run. Under vkd3d-proton it renders frames byte-identical to the SM 6.0
`min16float` modules, at the same speed, so sections 2 and 3's diagnosis does not hold on this platform. The 42-57 dB
gap against lsfg-vk's FP16 output is FP16 itself: changing nothing but dxc's optimisation level moves our own FP16
output by as much (53-59 dB), while the same change leaves FP32 at 72-78 dB. FP16 is 8-12% faster in quality mode and
about 1% in performance mode. Parity at the FP32 path's 72-75 dB is out of reach for any conversion, so nothing is
wired in.

## 1. Problem Statement

In the D3D12 port of LSFG 3.1:
- **FP32 variants** achieve **72–75 dB PSNR** compared to `lsfg-vk` Vulkan reference frames.
- **FP16 variants** (resource IDs 51..98, plus base shader 2) achieve only **42–57 dB PSNR**, with noticeable visual drift in coarse flow boundaries.
- **Performance:** In the current SM 6.0 conversion, FP16 is **not faster** than FP32 (and in some dispatches slightly slower).
- **Compilation with Native Linux DXC (`/usr/sbin/dxc`):** Fails with an internal LLVM assertion (`User.h:60`).

---

## 2. Root Cause Analysis: Why SM 6.0 FP16 Drifts & Lacks Speed

*Superseded under vkd3d-proton by section 7: there `min16float` runs as native 16-bit and gives the same bytes as a
true SM 6.2 conversion. What follows is the hypothesis as first written, kept for the record.*

### A. Semantic Mismatch: `min16float` vs. Vulkan IEEE 754 `Float16`
1. **Precision Relaxation vs. True IEEE 16-bit:**
   - In `lsfg-vk` (Vulkan), shaders execute directly from SPIR-V with the `Float16` capability (`VK_KHR_shader_float16`). Arithmetic is performed strictly in IEEE 754 binary16 (half-precision).
   - In `tools/lsfg-study/convert.py`, SPIR-V was translated via `spirv-cross --hlsl --shader-model 60` and compiled with `dxc -T cs_6_0`.
   - In HLSL Shader Model 6.0, 16-bit variables are represented as `min16float`. According to Direct3D specifications, `min16float` is only a *minimum precision hint*. On desktop GPUs (NVIDIA Turing/Ampere/Ada and AMD RDNA), drivers frequently promote `min16float` to full 32-bit floats or perform loose intermediate conversions.

2. **Exponential Error Compounding Across the 7-Scale Pyramid:**
   - LSFG 3.1 refines optical flow hierarchically across 7 scales ($i = 0..6$).
   - Scale 0 has an extent of $8 \times 4$ for a 1080p frame, while scale 6 has an extent of $480 \times 270$.
   - Any small numerical divergence in the coarse cost volume or flow estimation at scale 0 is magnified by a factor of $2^6 = 64\times$ when upsampled and accumulated towards native resolution.
   - In FP32, precision is high enough that rounding differences do not escape sub-pixel thresholds (yielding 72–75 dB). With `min16float` emulation, rounding errors in the neural network dense layers accumulate across 100 dispatches, resulting in 42–57 dB.

3. **Why `min16float` is Not Faster:**
   - True hardware FP16 acceleration requires packed 2-way SIMD (`half2` / `float16x2`) ALU operations.
   - Under SM 6.0 without `-enable-16bit-types`, the GPU does not issue 2-way packed half instructions. Instead, DXC and the driver insert extra unpack/pack conversions (`fto_min16` and `min16_to_f`) around 32-bit ALU instructions. This increases register pressure and instruction count, completely negating any bandwidth benefit.

---

## 3. Investigating Native SM 6.2 (`-enable-16bit-types`)

To achieve true hardware 16-bit operations in D3D12, HLSL requires:
- Target profile `cs_6_2` (or newer).
- Flag `-enable-16bit-types`.
- Native types `half` / `float16_t` (emitted by `spirv-cross --hlsl-enable-16bit-types`).

### The Native Linux DXC Assertion Crash

When compiling shaders with the native Arch Linux `/usr/sbin/dxc`, all shaders crash with:

```text
Error: assert((!HasHungOffUses || !getOperandList()) && "Error in initializing hung off uses for User")
File: /usr/src/debug/directx-shader-compiler/directx-shader-compiler/include/llvm/IR/User.h(60)
Func: User
```

#### Diagnostic Breakdown:
1. **Assertion Build in Native Linux Package:**
   - `/usr/sbin/dxc` on Arch Linux is built from source with `LLVM_ENABLE_ASSERTIONS=ON`.
   - The Windows official release binary (`OptiScaler/shaders/shader_tools/dxc.exe`) is built with `NDEBUG` (assertions compiled out), which is why `convert.py` succeeded under Wine.
2. **Trigger: Dynamic Resource Indexing & Inline Matrix Literals:**
   - `spirv-cross` translates SPIR-V matrix multiplication (`OpMatrixTimesVector`) into inline `half4x4(half4(...), ...)` constructor literals.
   - `spirv-cross` also emits helper functions (`spvTextureSize(Input[pc_Iteration], ...)`) where dynamically-indexed texture array elements are passed to helper functions.
   - In DXC, the AST codegen for User/PHINode operand allocation has an assertion check in `User.h(60)` that trips during GEP / matrix constructor generation in assertion builds.

---

## 4. Workarounds and Verification

*Section 7: with the release `dxc.exe` none of these were needed. All 48 modules compile as spirv-cross emits them,
`half4x4` constructors and resource helpers included; the assertion is the Arch package's assertion build only.*

1. **Decomposing `mul(vec, matrix)` into Vector Dot Products:**
   - Replacing:
     ```hlsl
     mul(v, half4x4(c0, c1, c2, c3))
     ```
     with:
     ```hlsl
     half4(dot(v, c0), dot(v, c1), dot(v, c2), dot(v, c3))
     ```
     bypasses the broken `half4x4` AST constructor in DXC.
   - Vector `dot(half4, half4)` compiles cleanly to native hardware `dot2add` / packed FP16 instructions.
2. **Inlining Resource Intrinsic Helpers:**
   - Inlining `Input[pc_Iteration].GetDimensions(...)` directly at call sites removes the intermediate function call with resource handle arguments.
3. **Compiling with NDEBUG DXC Binary:**
   - Compiling via release DXC (without `LLVM_ENABLE_ASSERTIONS`) generates valid DXIL without tripping the frontend assertion.

---

## 5. Output Directory & Status

- Output directory: `Tmp/lsfg-study/dxil16/` (contains generated HLSL and metadata).
- For production D3D12 integration, ensure the runtime checks:
  ```cpp
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4 = {};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)))) {
      bool native16Supported = options4.Native16BitShaderOpsSupported;
  }
  ```
  If `Native16BitShaderOpsSupported` is false, fallback to the FP32 pipeline (resource IDs 3..26) rather than SM 6.0 `min16float` to maintain visual quality and avoid performance degradation.

---

## 6. What FP16 could still buy after the barrier waves (2026-10-13)

Not attempted in the performance step (`OPTI_INTEGRATION.md`, "GPU cost, second step"). Native 16-bit needs the
converter to emit `float16_t` for `cs_6_2` with `-enable-16bit-types`, plus the section 4 workarounds. That is
`convert.py` / `LsfgShaderLibrary` work, not recording work. The recording side needs only the variant ids (+48),
which `VariantRid` already handles. The per-wave timing bounds what it could buy (`tools/lsfg-study/ctxbench
--wave-timing`, RTX 4090 at full clock, 1691x1312, quality, FP32, 0.445 ms per generated frame):

- **Out of FP16's reach, about 230 µs:**
  - the ingest copy, 15 µs;
  - mipmaps, 18 µs, and generate, 31 µs: base shaders with no FP16 variant;
  - a floor of ~4 µs per wave, whatever the wave holds, across 42 waves. The waves of a few dozen thread groups
    cost exactly that.
- **Where it could act, about 240 µs:** the core-shader waves that are compute-heavy.
  - the alpha waves, 63 µs;
  - the beta chain with gamma/delta level 0, 62 µs;
  - gamma2/3 at level 2, 39 µs;
  - the level-6 waves, ~80 µs.
  Even a 2x on those alone would leave about 0.33 ms.
- **Parity is the condition, and it is not met yet.** FP16 is Lossless Scaling's own lower-precision mode, so it is
  judged against lsfg-vk's FP16 output, not against FP32. The `min16float` conversion reaches only 42.8-56.6 dB
  there (harness README). A native conversion would have to reach the 72-75 dB the FP32 path reaches before its
  timing means anything.

---

## 7. Native 16-bit, built and measured (2026-10-14)

### What was built

- `convert.py --fp16-native` (study only): every module that declares `OpCapability Float16` goes through
  `spirv-cross --hlsl --shader-model 62 --hlsl-enable-16bit-types`, the same register remap, and the release
  `dxc.exe` (1.9.2602.17) under Wine with `-T cs_6_2 -enable-16bit-types -E main -O3`. Output goes to
  `<target>/fp16-native/` (`<rid>.cso`, `hlsl/`, `manifest.json` with the flags, dxc version and DLL hash), beside the
  default output and never in place of it. Nothing in OptiScaler reads it.
- **The FP16 modules are exactly ids 51..98.** That is the `OpCapability Float16` scan of the installed
  `lsfg-vk.dll` (SHA-256 `d22b2d3f...`), pinned in `tests/lsfg-shader-library`. Base shader 2 is not one of them. It is
  the `generate` for a 16-bit output image, declares no Float16, and converts to the same HLSL as 1.
- **All 48 compile with no workaround.** Without the remap, 5 fail with overlapping registers (60, 69, 79, 84, 93):
  that is the remap's job, not a 16-bit matter. The DXIL says `Use native low precision` where the SM 6.0 modules
  say `Minimum-precision data types`.
- The default output is unchanged. A fresh `convert.py --fp16-native` run into a scratch cache gave all 99 default
  `<rid>.cso` and `layout.json` byte-identical to the PCSX2 cache the old converter made, and the same file set plus
  `fp16-native/`.

### Parity: byte-identical to `min16float`

The harness (`tools/lsfg-study/harness`, now with `--shaders`) ran the 1920x1080 translation and rotzoom sequences
with the FP16 variants and the native modules. References are lsfg-vk's own FP16 frames, from `lsfg-vk-cli debug`
without `-a`: `Tmp/lsfg-study/ref` for x2, `ref_m3`, `ref_m4`, and the perf/flow 0.5 renders under
`out/comparisons/`. `ref_m3/*/1_2.dds` is byte-identical to the FP16 x3 `lsfg-vk-cli` render in `out/comparisons/`,
and `ref/*/1.dds` to `ref_m4/*/1_2.dds`. PSNR in dB, max diff in levels out of 255. RGB, `compare.py`, threshold 2.

| Frame | translation native = min16 | -O3 -Gis | -Od | rotzoom native = min16 | -O3 -Gis | -Od |
|---|---|---|---|---|---|---|
| x2 t=1/2 | 50.08 / 47 | 49.42 / 46 | 50.33 / 48 | 54.07 / 57 | 53.24 / 65 | 55.90 / 42 |
| x3 t=1/3 | 56.98 / 32 | 56.28 / 30 | 56.54 / 37 | 54.94 / 76 | 54.35 / 65 | 57.02 / 54 |
| x3 t=2/3 | 56.64 / 34 | 56.08 / 44 | 55.92 / 49 | 56.32 / 43 | 55.79 / 52 | 58.06 / 28 |
| x4 t=1/4 | 54.04 / 36 | 53.90 / 39 | 53.63 / 30 | 55.73 / 73 | 55.39 / 75 | 59.14 / 51 |
| x4 t=2/4 | 50.08 / 47 | 49.42 / 46 | 50.33 / 48 | 54.07 / 57 | 53.24 / 65 | 55.90 / 42 |
| x4 t=3/4 | 53.25 / 38 | 53.18 / 42 | 52.84 / 48 | 57.41 / 50 | 56.88 / 42 | 59.31 / 28 |
| x2 perf, flow 0.5 | 45.51 / 85 | 46.22 / 53 | 47.80 / 43 | 42.76 / 70 | 42.33 / 68 | 43.38 / 62 |
| FP32 for context (x2 / x3 2/3) | 74.59 / 3, 73.80 / 4 | | | 71.91 / 7, 74.97 / 2 | | |

- **Native and `min16float` give the same bytes** on all 14 frames, and in `ctxbench`'s 15-frame moving sequence at
  both sizes and both modes (FNV hashes). The numbers in that column are the `min16float` numbers of the harness README
  and `MFG_PRESENTER.md`.
- **Why:** `VKD3D_SHADER_DUMP_PATH` shows both DXIL flavours becoming SPIR-V with `OpTypeFloat 16` and no
  `RelaxedPrecision`. vkd3d-proton lowers minimum precision to native 16-bit on a device that has it, so the GPU runs
  the same 16-bit arithmetic either way. Section 2's "the driver promotes `min16float` to 32-bit" does not happen here.
- **The gap is FP16's sensitivity to operation order, not a conversion error.** Same HLSL, same harness, only
  dxc's flags changed:

  | | translation x2 | rotzoom x2 | translation x3 2/3 | rotzoom x3 2/3 |
  |---|---|---|---|---|
  | our FP16, -O3 vs -Od | 54.24 / 40 | 54.71 / 57 | 59.24 / 34 | 56.40 / 44 |
  | our FP16, -O3 vs -O3 -Gis | 53.45 / 29 | 53.17 / 66 | | |
  | our FP32, -O3 vs -Od | 72.70 / 4 | 72.86 / 5 | 78.01 / 2 | 75.88 / 2 |
  | lsfg-vk FP16 vs lsfg-vk FP32 | 47.81 / 55 | 55.32 / 48 | | |
  | our FP16 vs our FP32 | 51.10 / 44 | 53.53 / 60 | | |

  Reordering FP16 operations moves the output as far as the whole gap to lsfg-vk. The same reordering in FP32 stays
  at 72-78 dB, and FP32 -Od against lsfg-vk FP32 is 75.1-78.5 dB. The 72-75 dB target would need the same 16-bit
  rounding, operation for operation, after two different driver compilers: NVIDIA's for lsfg-vk's SPIR-V, and for
  dxil-spirv's SPIR-V here (for DXIL on Windows). No conversion can promise that. `-Od` is 0.6-3.4 dB closer on
  rotzoom and up to 0.7 dB further on translation's quality frames: noise of the same kind, not a fix.

### Speed: `ctxbench`, RTX 4090 at full clock

`ctxbench` (now with `--fp16`), FP32 vs native FP16 vs `min16float`. Each case ran 200 warm-up and 1500 timed
generated frames, flow 1.0, direct output, in two interleaved rounds. nvidia-smi read 2730-2745 MHz graphics, 10501 MHz
memory and P0 throughout. PCSX2 was held off with its lock. Times are p50 in ms per generated frame, round 1 / round 2.

| Size, mode | FP32 | FP16 native | FP16 `min16float` | FP16 vs FP32 |
|---|---|---|---|---|
| 1691x1312 quality | 0.4485 / 0.4465 | 0.4024 / 0.3983 | 0.4045 / 0.3932 | -10% (~47 µs) |
| 1691x1312 performance | 0.2447 / 0.2467 | 0.2447 / 0.2519 | 0.2447 / 0.2488 | ~0 |
| 3440x1440 quality | 0.7923 / 0.7926 | 0.7301 / 0.7291 | 0.7291 / 0.7299 | -8% (~63 µs) |
| 3440x1440 performance | 0.4751 / 0.4751 | 0.4710 / 0.4700 | 0.4710 / 0.4708 | -1% |

- Per wave (`--wave-timing`, 1691x1312 quality, sum of interval medians 449 -> 395 µs), the time comes off the waves
  that hold gamma2/gamma3, together with delta2/delta3 (levels 0-2), epsilon2/epsilon3 (levels 4-6) or beta2/beta3 (the
  pre-pass overlap): 2-10 µs each. The alpha3 wave gives 4 µs. Waves of a few dozen groups stay at their ~4 µs floor,
  and the ingest, mipmaps and generate do not move. About 50 of the ~240 µs that section 6 put within FP16's reach.
- Performance mode halves the feature channels, and its dispatches are already near the per-wave floor, so 16-bit
  arithmetic has almost nothing to take.

### Decision

- **Not wired.** The parity condition fails, and not for want of a better conversion. On this platform the native
  modules are also indistinguishable from the `min16float` ones the cache already holds. So a runtime switch, an
  `OPTIONS4.Native16BitShaderOpsSupported` / SM 6.2 check and a cache manifest would buy nothing measurable here.
  `Settings::fp16` stays `false` with no ini key.
- **If FP16 is ever wanted anyway**, the question is a different one: is our FP16 frame acceptable as *an* FP16
  rendering of LSFG? It is as far from lsfg-vk's FP16 as lsfg-vk's FP16 is from its own FP32. That is a judgement
  on images, for the user, worth 8-12% of LSFG's GPU time in quality mode and about 1% in performance mode.
  - On Proton the existing `min16float` modules already deliver it.
  - The native modules matter only where a driver honours `min16float` differently, which may be native Windows.
    That is untested; Rafael's RTX 3060 is the machine for it. Wiring it would then need: the SM 6.2 and
    `Native16BitShaderOpsSupported` checks before PSO creation, a fall back to FP32 when PSO creation fails, a cache
    marker that an old cache cannot fake, and an ini key under the four-point rule.
- Artifacts: `~/.cache/lsfg-fp16-artifacts/` holds the parity PNGs, metrics, vkd3d-proton shader dumps and
  `ctxbench` logs. They are outside git and derived from the user's DLL.
