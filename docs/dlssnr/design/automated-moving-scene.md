# Automated moving-scene NR regression fixture

## Decision and boundary

Extend `tests/dlssnr-loopback` with `--moving-scene`. This is a test application,
not new production behaviour. It drives the built OptiScaler DLL's public D3D12 NGX
exports, selecting native DLSS as the upscaler and HookMethod=1 for NR. It does not
substitute a private forwarder-only pipeline for the production host.

The existing deterministic `PoseForFrame`, Halton jitter and `scene.hlsl` become
an executable cube scene. A graphics PSO writes linear RGBA16F colour, RG16F
**pixel-unit, current-to-previous** motion and R32_FLOAT device depth, with a
separate D32 depth attachment for visibility. Current/previous transforms are
unjittered; jitter affects rasterization only. Fixed 640x360 render/output (DLAA)
keeps captures bounded. The cube uses hard face normals and has a static clear
background; this is deliberately a narrow, opaque-object scenario.

## Trials, completion and evidence

Run fresh processes in order NR off, on, on, off. Every process renders the same
64 warmup frames and 16 measured frames. The initial frame resets DLSS history;
NR's production initialization/settling is allowed during warmup. Warmup is excluded
from timing and metrics; a missing NR composition in either enabled process fails.
Every recording is submitted and a bounded fence proves completion before mapping,
reusing, freeing or presenting resources. Unknown completion terminates the process
without unwinding GPU-owned resources.

Two timestamps bracket only the public EvaluateFeature call (DLSS plus NR when
enabled). Rendering, upload, readback and CPU waits are outside this interval.
Report individual samples and nearest-rank p50/p95 per trial, along with the DLL,
forwarder, model, shader and core hashes and runtime/environment provenance.
These numbers are descriptive GPU work durations, not FPS or isolated NR kernel
cost; clocks, thermal state and other GPU users can confound A/B differences.
No timing threshold can fail the fixture on an otherwise healthy machine.

Save the sixteen measured frames as raw half-float colour/output/motion and float
depth, with matrices/jitter/frame indices in a manifest. Analysis produces preview
PPMs and a JSON report. The runner removes stale trial directories first. It never
writes to a game directory or downloads models.

## Independent checks and negative controls

1. Require all creates/evaluations and fences to succeed, all expected files and
   frame indices to exist, all pixels to be finite, and source/depth/motion hashes
   to agree across the four independent processes.
2. Reconstruct local geometry from GPU device-depth readback using inverse current
   view-projection/model, then project through the previous model/view-projection.
   Compare to the GPU motion readback at interior sampled pixels, accounting for
   raster jitter. This inverse-depth oracle is independent of the shader's forward
   motion computation; reject insufficient coverage, flat motion and frozen inputs.
3. Demand a nonzero NR-on versus NR-off pixel difference beyond the repeated-on
   difference, plus production composition evidence. This is a processing coverage
   gate, not a quality threshold.
4. Report RGB MAE, repeated-on error and a motion-compensated temporal difference
   of the NR edit (on minus off), with in-bounds, depth-consistent surface matches.
   No assertion claims that a smaller temporal metric means better pictures.
5. Exercise the analyzer against zeroed motion and processing-disabled output;
   both must be rejected. Host tests also supply analytically known moving planar
   samples and verify percentile definitions, malformed data and finite checks.

A pass establishes the tested D3D12 production route processes moving input with
geometrically consistent guides. It does not cover Vulkan, D3D11 transport, FG,
transparent objects, disocclusion quality, Windows debug-layer validation, game
compatibility, perceptual quality or a performance benefit.
