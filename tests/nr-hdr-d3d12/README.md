# HDR present conversion on D3D12

Runs the production conversion class, Shader_Dx12 descriptor/pipeline helpers and compiled HDR shader on a real
D3D12 device. Only application configuration/logger/shutdown state are shimmed. No NGX, OptiScaler proxy or game.

Mandatory checks: 8,192 HDR10 and scRGB pixels return bit-exact through decode/encode, including alpha; a known
FP16 edit preserves the exterior and matches the independent float64 PQ encode within one code (scRGB exactly).
The original includes every ten-bit code, wide-gamut colours, signed scRGB, highlights and subnormals. Resource
aliasing and invalid profile/output contracts are rejected before descriptor writes. Each recording is fenced
before reuse. Native D3D12 debug-layer errors fail when that layer is available.

The harness also creates a real ten-bit flip swapchain, queries PQ presentation support, sets PQ only when
advertised and presents an uploaded ten-bit frame. Lack of PQ support is an explicit presentation SKIP, separate
from mandatory texture conversion PASS. It queries the production display-white implementation. It does not force
DXVK_HDR or change desktop HDR/VRR settings. Texture PASS is not evidence of native Windows API validity or HDR
scanout, and the swapchain probe does not run the NR model.

Run only with no game mappings, under the shared lock:

```sh
flock -o "$HOME/.cache/retoque-wt/msvc-prefix.lock" python3 tests/nr-hdr-d3d12/run.py
```

The runner writes `artifacts/{build.log,run.log,result.json}` and stages only inside this suite. Rafael can run
`artifacts/run/hdr-harness.exe` with the native D3D12/DXGI system libraries (omit the Wine runtime DLLs) and the native
debug layer installed; its INFO/ERROR output must be retained. This still needs a game run for visual quality,
actual present-hook integration, HDR metadata, toggle/resize and UI preservation.
