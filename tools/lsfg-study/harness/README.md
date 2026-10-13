# Standalone LSFG 3.1 D3D12 harness

Quality mode, SDR RGBA8, flow scale 1.0; multipliers 2 and 3. Loads the user's
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

Inputs must have matching dimensions, 64..16384 per axis. A folder accepts
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
not claim forced native 16-bit arithmetic or an FP16 performance measurement.

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
comparison is pending the Part A images and `../compare.py`. Explicit Vulkan
validation loads but exits during vkd3d device creation before harness resources
are allocated; it is not a validation pass.
