# Standalone LSFG 3.1 D3D12 harness

Quality/performance modes, SDR RGBA8, flow scales 0.25..1.0; multipliers 2 and 3. Loads the user's
extracted DXIL (no shader/model redistribution), executes all 34 preprocessing
and 66 generation dispatches, and saves RGBA8 PNGs. No OptiScaler or game required.

From the `study/lsfg-d3d12` worktree:

```sh
python tools/lsfg-study/harness/run.py --build-only
python tools/lsfg-study/harness/run.py --run-only previous.png current.png
python tools/lsfg-study/harness/run.py --run-only --multiplier 3 /path/to/numbered-png-folder
python tools/lsfg-study/harness/run.py --run-only --multiplier 3 --fp16 /path/to/numbered-png-folder
```

Without either mode flag, the script builds and runs. It uses MSVC 14.44/Windows
SDK through `~/.local/opt/msvc/bin/x64/cl`, the owned
`~/.local/opt/msvc-wineprefix`, native MSVC runtime and vkd3d-proton from Proton.
It checks for MSBuild/cl/dxc before each compile/run, redirects Wine output to
files (no pipes), and stops its Wine server if a command times out.

Default artifact root is
`~/Projects/personal/OptiScaler_DLSSNR/Tmp/lsfg-study/` (`--artifacts` overrides).
DXIL comes from `dxil/`; executable, objects, runtime DLLs, build log and shader
cache go in `harness/`; PNGs and run log go in `out/` (`--output` overrides).
Nothing generated is written to the worktree.

Inputs must have matching dimensions, 64..16384 per axis; both flow-scaled
dimensions must remain at least 64. A folder accepts
numerically named `0.png`, `1.png`, etc., sorted numerically. Output `F_K.png` is
fraction K/multiplier between input F-1 and F. For two paths use `1_1.png` at 2x,
or `1_1.png` and `1_2.png` at 3x. Frame-zero outputs are diagnostic warm-up.
Source slots rotate modulo 2; alpha feature history rotates modulo 3, following
lsfg-vk's iteration counter. All history starts at zero. For a reference match,
feed exactly the same whole sequence; comparing isolated last pairs changes
history. Alpha is preserved from the shader, including border sampling; WIC decodes/encodes
without premultiplying or adding a color-space transform.

`--fp16` selects resource IDs +48 for the 24 core shaders (base shaders stay 0/1).
The supplied cs_6_0 conversions express these as `min16float`; this option does
not claim forced native 16-bit arithmetic. Timing rows labeled FP16 measure
these variants. `--performance` adds 24 to core shader IDs and halves the
feature-channel counts; `--flow-scale` scales the internal pyramids while
source and output remain at full resolution.

## D3D12 contract

Root signatures come from DXIL reflection, including the compiler-assigned
register of `type_PushConstant_PC`; that buffer uses root constants. Other
bindings each have their own descriptor table at register zero in the original
Vulkan binding's space. Descriptor array lengths are checked against the graph.
Texture2D arrays of descriptors are distinct from layered Texture2DArray views.
R8/RGBA8 UNORM and RGBA16 FLOAT support is queried before allocation.

Each pass has immutable, full-length descriptor tables. Unused array elements
have typed null views. This matters for gamma/delta/epsilon: their input and
output arrays cover the same set of resources, while each dispatch reads the
previous level and writes the current level. Binding all images in both roles
would violate D3D12 state requirements. The active resources transition to
NON_PIXEL_SHADER_RESOURCE or UNORDERED_ACCESS; UAV barriers order writes.
Uploads/readbacks use GetCopyableFootprints and aligned row pitches. CBVs are
256 bytes. CPU constant updates, resource reuse and destruction follow completed
fences. There is no placed-resource aliasing or asynchronous queue overlap.

See Microsoft's [resource barrier rules](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
and [root signature syntax](https://learn.microsoft.com/en-us/windows/win32/direct3d12/specifying-root-signatures-in-hlsl).

## Native Windows

In an x64 MSVC developer shell, compile in an **artifact directory**:

```bat
cl /nologo /std:c++20 /EHsc /MD /W4 /O2 /DUNICODE /D_UNICODE C:\src\tools\lsfg-study\harness\main.cpp /Felsfg-harness.exe /link d3d12.lib dxgi.lib dxguid.lib windowscodecs.lib ole32.lib
lsfg-harness.exe --shaders C:\study\dxil --out C:\study\out --multiplier 3 --debug --gpu-validation C:\study\inputs
```

Place `dxcompiler.dll` beside the EXE and install the VC++ runtime. Use the system
D3D12 runtime on Windows: do not copy the Wine artifact directory's `d3d12.dll`
or `d3d12core.dll`. Debug/GBV flags require Windows Graphics Tools; failure to
obtain the layer is fatal. Debug queue errors are fatal after GPU completion.

## Validation status

MSVC build and real RTX 4090/vkd3d-proton runs pass at 256x256, FP32 2x/3x and
FP16 variants 3x; a 257x193 constant sequence also passes (after warm-up,
16-pixel-inset interior is byte-exact). D3D12 debug layer is unavailable in the current Wine prefix
(`0x887a002d`); native Windows/GBV validation is still required. Reference
comparisons and timing results are below. Explicit Vulkan
validation loads but exits during vkd3d device creation before harness resources
are allocated; it is not a validation pass.

## GPU timing and Vulkan comparison (2026-10-13)

```sh
# Build once, then reproduce all 16 cases (sequential, about a few minutes):
python tools/lsfg-study/harness/run.py --build-only
python tools/lsfg-study/harness/benchmark.py
# One case:
python tools/lsfg-study/harness/run.py --run-only --benchmark --width 3440 --height 1440 --performance --fp16 --flow-scale 0.5 --warmup 100 --iterations 400
```

D3D12 uses EndQuery(TIMESTAMP), the direct queue's GetTimestampFrequency and
fenced ResolveQueryData readback. Each case has **100 warm-up + 400 measured
iterations in each of two sweeps**: four timestamp queries for total/main-phase
time, then 204 queries for total/main and all 100 individual dispatches. A pass
interval includes its transitions, dispatch and trailing UAV barriers. GPU total
includes preprocessing plus one generated frame (2×), excluding uploads, image
readback, query resolve and CPU work. The main-only median/p95 is also in JSON.
Median averages the middle two samples; p95 uses nearest rank. All 12,800 saved
samples were checked: per-pass intervals sum to no more than the enclosing total,
and raw samples reproduce the summary statistics. Instrumenting individual passes
added **0.038–0.040 ms** to total medians here; the table uses the four-query sweep.

The native command for every matching case was:
`lsfg-vk-cli benchmark -w W -h H -m 2 -t 5 -f SCALE`, adding `-p` for performance
and `-a` for FP32. Its **Time per iteration is a wall-clock mean**, not GPU
median/p95. D3D wall mean includes re-recording, submit/fence waits, query readback
and sample bookkeeping; Vulkan reuses recorded command buffers. The D3D harness
also serializes its graph conservatively, while Vulkan groups independent passes.
These columns compare the current implementations, not isolated API overhead.

RTX 4090, driver 615.78.08, Wine + vkd3d-proton 3.1.0 (`2230755878b0199`), native
lsfg-vk 2.0.0. Clocks were **unlocked**, desktop running, cases sequential; observed
post-Vulkan graphics clocks were 2730–2760 MHz, memory 10501 MHz. Native CLI warned
that real-time scheduling was unavailable. D3D benchmark input is zero-initialized;
the native benchmark does not upload image content. These are synthetic pipeline
measurements, not measurements on game frames. No system clock/scheduling settings
were changed. Every duration below is **milliseconds**.

| Resolution | Mode | Variant | Flow | D3D GPU median / p95 | D3D wall mean | Vulkan wall mean |
|---|---|---|---:|---:|---:|---:|
| 1920×1080 | quality | FP32 | 1 | 0.504 / 0.549 | 1.243 | 0.43 |
| 1920×1080 | quality | FP32 | 0.5 | 0.380 / 0.414 | 0.758 | 0.30 |
| 1920×1080 | quality | FP16 | 1 | 0.472 / 0.521 | 0.689 | 0.44 |
| 1920×1080 | quality | FP16 | 0.5 | 0.342 / 0.381 | 0.908 | 0.30 |
| 1920×1080 | performance | FP32 | 1 | 0.334 / 0.386 | 0.880 | 0.28 |
| 1920×1080 | performance | FP32 | 0.5 | 0.270 / 0.280 | 0.500 | 0.21 |
| 1920×1080 | performance | FP16 | 1 | 0.332 / 0.389 | 0.863 | 0.29 |
| 1920×1080 | performance | FP16 | 0.5 | 0.268 / 0.300 | 0.821 | 0.21 |
| 3440×1440 | quality | FP32 | 1 | 0.830 / 0.871 | 1.621 | 0.72 |
| 3440×1440 | quality | FP32 | 0.5 | 0.473 / 0.519 | 1.134 | 0.39 |
| 3440×1440 | quality | FP16 | 1 | 0.786 / 0.831 | 1.549 | 0.79 |
| 3440×1440 | quality | FP16 | 0.5 | 0.434 / 0.489 | 1.043 | 0.39 |
| 3440×1440 | performance | FP32 | 1 | 0.509 / 0.539 | 1.177 | 0.44 |
| 3440×1440 | performance | FP32 | 0.5 | 0.328 / 0.377 | 0.964 | 0.27 |
| 3440×1440 | performance | FP16 | 1 | 0.510 / 0.545 | 1.104 | 0.46 |
| 3440×1440 | performance | FP16 | 0.5 | 0.327 / 0.375 | 0.797 | 0.28 |

Raw evidence is outside git under `Tmp/lsfg-study/out/benchmarks/`: `results.json`
contains configuration, EXE hash and GPU telemetry; each named case has
`timing.json`, `timing-passes.csv` (100 rows, shader/level/median/p95),
`timing-samples.csv` (800 rows), `run.log` and `vulkan.log`. D3D total GPU medians
span 0.268–0.830 ms. Half flow and performance mode reduce cost; these runs show
little consistent benefit from selecting the FP16 variants in performance mode.

Largest individual dispatches at 3440×1440, quality FP32, flow 1.0:

| Pass | Subiteration | GPU median ms | GPU p95 ms |
|---|---:|---:|---:|
| generate | 0 | 0.050176 | 0.051200 |
| alpha3 | 6 | 0.031744 | 0.031744 |
| alpha1 | 6 | 0.029696 | 0.029696 |
| gamma2 | 6 | 0.029696 | 0.030720 |
| gamma3 | 6 | 0.029696 | 0.030720 |
| delta2 | 2 | 0.029696 | 0.030720 |
| delta3 | 2 | 0.029696 | 0.030720 |
| mipmaps | 0 | 0.026624 | 0.027648 |

For all per-pass rows, open the corresponding `timing-passes.csv`; the full
instrumented total is recorded separately rather than summing per-pass medians.

## Reference comparison

```sh
uv run --with pillow --with numpy python tools/lsfg-study/harness/compare_reference.py
```

This invokes Part A's `../compare.py` on the 1920×1080 translation and rotation/
zoom inputs. Quality 2× FP16 uses the original `Tmp/lsfg-study/ref/` images.
Other rows use additional native CLI renders with matching precision/mode/flow,
stored under `out/comparisons/` without replacing Part A's references. Metrics
are RGB, full image, 0–255 channel values. Input order and iteration start match.
The native exporter saves only the last acquire: **3× rows compare the 2/3 frame**;
the 1/3 frame is generated by D3D12 but lacks a native reference in this exporter.

| Input | Variant | Multiplier | Mode / flow | PSNR dB | Max diff / 255 | Pixels >2 |
|---|---|---:|---|---:|---:|---:|
| translation | FP16 | 2 | quality / 1 | 50.075 | 47 | 1.427% |
| translation | FP32 | 2 | quality / 1 | 74.592 | 3 | 0.000% |
| translation | FP16 | 3 | quality / 1 | 56.636 | 34 | 0.371% |
| translation | FP32 | 3 | quality / 1 | 73.800 | 4 | 0.000% |
| translation | FP16 | 2 | performance / 0.5 | 45.505 | 85 | 4.815% |
| rotzoom | FP16 | 2 | quality / 1 | 54.066 | 57 | 0.694% |
| rotzoom | FP32 | 2 | quality / 1 | 71.911 | 7 | 0.003% |
| rotzoom | FP16 | 3 | quality / 1 | 56.317 | 43 | 0.401% |
| rotzoom | FP32 | 3 | quality / 1 | 74.965 | 2 | 0.000% |
| rotzoom | FP16 | 2 | performance / 0.5 | 42.764 | 70 | 3.218% |

FP32 quality agrees closely with the matching Vulkan shaders (71.9–75.0 dB).
FP16 variants are less close (42.8–56.6 dB across these tests); strict FP16 parity
remains open. Their cs_6_0 minimum-precision conversion does not guarantee the
same arithmetic/rounding as the native SPIR-V. That is a possible source of the
remaining differences, not a demonstrated root cause. Full metrics, reference
hashes, amplified diff PNGs and both runners' logs are in `out/comparisons/`.
