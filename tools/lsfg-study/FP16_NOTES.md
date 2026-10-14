# LSFG 3.1 FP16 Investigation Notes

## 1. Problem Statement

In the D3D12 port of LSFG 3.1:
- **FP32 variants** achieve **72–75 dB PSNR** compared to `lsfg-vk` Vulkan reference frames.
- **FP16 variants** (resource IDs 51..98, plus base shader 2) achieve only **42–57 dB PSNR**, with noticeable visual drift in coarse flow boundaries.
- **Performance:** In the current SM 6.0 conversion, FP16 is **not faster** than FP32 (and in some dispatches slightly slower).
- **Compilation with Native Linux DXC (`/usr/sbin/dxc`):** Fails with an internal LLVM assertion (`User.h:60`).

---

## 2. Root Cause Analysis: Why SM 6.0 FP16 Drifts & Lacks Speed

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
