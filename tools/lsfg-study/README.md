# LSFG on D3D12 (study, branch study/lsfg-d3d12)

Running Lossless Scaling's frame generation (LSFG 3.1) on D3D12 inside OptiScaler, from the user's own Lossless Scaling
install (`lsfg-vk` beta branch, `lsfg-vk.dll`). Generated files go to `Tmp/lsfg-study/` (outside git).

1. `extract.py <lsfg-vk.dll> Tmp/lsfg-study/spv`: the 99 SPIR-V modules, by resource id (0..98: mipmaps, generate,
   then alpha/beta/gamma/delta/epsilon in quality/performance x FP32/FP16; the id table is lsfg-vk's `library.cpp`).
2. `convert.py Tmp/lsfg-study/spv Tmp/lsfg-study`: spirv-cross to HLSL SM 6.0, each Vulkan binding moved to its own
   register space (binding N -> `register(x0, spaceN)`, so arrays don't overlap), dxc to DXIL. `layout.json` holds the
   per-shader binding map for the root signatures. Status 2026-10-13: 99/99 compile.
3. Next: a D3D12 harness that runs the LSFG 3.1 pass graph (lsfg-vk `pipelines/v3_1.cpp`) on two frames and writes
   the generated one, checked against `lsfg-vk-cli debug` on the same frames.

Needs: `spirv-cross` (Arch extra), `uvx --with pefile`, dxc from `OptiScaler/shaders/shader_tools/`.

`convert.py --fp16-native` also writes SM 6.2 native 16-bit conversions of the FP16 modules (ids 51..98) to
`<target>/fp16-native/`, for study. Nothing reads them: `FP16_NOTES.md` section 7 says why.
