# DLSS-NR loopback harness

A deterministic D3D12 application that drives the production Neural Rendering path without a
game. It exists because every NR measurement so far came from launching a title and reading a
log, which gives no control over the scene, no ground truth for motion, and no way to compare
two runs pixel by pixel.

## Why this is not another unit test

`tests/nr-*` exercise decision logic with fakes: no NGX, no D3D12, no shader. This harness is
the opposite. It creates a real D3D12 device and swapchain, renders a real scene, and calls
**`NVSDK_NGX_D3D12_EvaluateFeature` exported by the production `OptiScaler.dll`** — one of the
four NR call sites, the same entry a game reaches. Nothing is mocked below that line.

## What it controls that a game cannot

- **Motion vectors are analytic.** The camera path and the geometry are ours, so the previous
  and current clip-space positions are both known and the vector is computed, not estimated.
  A game's vectors are an input we have to trust; here they are ground truth.
- **The scene is deterministic.** Frame N is identical across runs — same camera, same
  animation phase, driven by a frame counter and never by wall-clock time. Two runs are
  therefore comparable pixel by pixel, which is what an image-quality claim requires.
- **The whole matrix is reachable from one process.** Working scale, stage, resolution changes
  and flag combinations can be swept without a human opening a game.

## What it deliberately does NOT prove

The scene is synthetic. The NR model is a relighting network trained on real rendered content,
so this harness is evidence about **cost, lifetime, determinism and regression** — not about
whether the picture looks better in a real title. An image difference here means the pass
changed the frame, not that it improved it. Visual judgement stays in-game.

## Status

`python3 tests/dlssnr-loopback/run.py` builds the harness with the msvc-wine toolchain, sets up
a disposable runtime prefix with vkd3d-proton borrowed from the local Proton install, links the NR
kit (forwarder, model, `_nvngx.dll`) and a donor game's DLSS/Streamline DLLs beside the production
`OptiScaler.dll`, and runs it. It refuses to start while `build-local.sh` holds the compiler prefix.

What works: a real D3D12 device on the GPU, all `NVSDK_NGX_D3D12_*` exports resolved from the
production DLL, `Init` and `CreateFeature` (DLSS super-sampling) succeeding, and thousands of
`EvaluateFeature` calls across a scripted sweep of render extents. The NR module is reached — its
spatial contract, guides and exposure scan all log — and the post-stage settling gate fires once on
the cold start, as designed.

**What does not work yet: the NR feature itself.** Its `CreateFeature` returns `0xBAD00002`
(`FAIL_PlatformError`), the module latches `it already failed this session`, and no frame is ever
composed. `_nvngx.dll` matches the driver byte for byte, and neither `nvngx_dlss.dll` nor the
eleven Streamline DLLs change the result, so those three explanations are ruled out; the cause is
still open. Because of this the runner **fails by design** with `ZERO COVERAGE: the NR pass never
composed a frame` rather than passing on the upscaler alone — the same rule the Vulkan harness
enforces. The registry entry in `tests/suites.toml` says so.

Still to do, in order: find what the model needs that this process does not provide; then the
deterministic scene (`scene.hlsl` is written, not yet wired); then frame dumps for run-to-run
comparison.

A stage that cannot reach its own instrumentation must fail loudly rather than pass quietly;
that is the `ZERO COVERAGE` rule the Vulkan harness established and this one inherits.
