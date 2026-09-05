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

Stage 1 (current): device, swapchain, deterministic scene, depth, analytic motion vectors.
Stage 2: NGX feature creation and per-frame evaluate through OptiScaler.
Stage 3: assertions and frame dumps for run-to-run comparison.

A stage that cannot reach its own instrumentation must fail loudly rather than pass quietly;
that is the `ZERO COVERAGE` rule the Vulkan harness established and this one inherits.
