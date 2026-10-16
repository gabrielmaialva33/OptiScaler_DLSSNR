# Automated Streamline input regression on D3D12

One command, no game, deployment or changes to Steam settings:

```sh
python3 tests/fg-inputs-d3d12/run.py
```

The runner executes `fg-game-camera` and `fg-tag-extents` under host sanitizers, then builds
a standalone Windows executable with the local MSVC toolchain and runs it with installed
Proton's D3D12/DXGI libraries in a separate Wine prefix. It acquires the shared compiler lock,
refuses an active compiler or mapped game, and keeps all generated files under `artifacts/`.
Missing prerequisites, a timeout, missing coverage or an unexpected control result fail the run.

The executable compiles the actual production camera-construction and resource-tag adapter
statements, the owned camera cache and the real Streamline SDK types:

- Four DirectXMath camera cases: perspective, asymmetric projection, reversed depth and
  orthographic projection. Exact matrix/metadata forwarding, projection inverse and temporal
  reprojection of known scene points are checked against scalar math. Invalid replacement
  constants must discard the previous camera and use the reconstructed fallback.
- Real D3D12 textures at two padded sizes: 64 moving extents cover depth, motion, HUD-less and UI
  tag roles. The native resource description flows through the production adapter; its returned
  origin drives a GPU crop, fence completion and readback. Every pixel is compared with the
  independently requested region of the source pattern. Full-texture fallback and invalid
  extents are checked too. Native debug-layer errors fail when the layer is available.
- Three separately compiled negative controls remove camera forwarding, move it before the
  backend's final camera metadata assignments, or remove origin forwarding.
  They must exit with the specific expected assertion. A broken fixture cannot silently pass.

`artifacts/result.json` records verdicts, source hashes, commit/dirty state, runtime and limits;
per-case build/run logs explain failures. `artifacts/run/current.exe` can also run on native
Windows with the MSVC runtime and system D3D12/DXGI (omit the staged Wine graphics DLLs).

This is a production-slice integration fixture, **not the complete OptiScaler DLL or the real
DLSS-G plugin**. Camera math runs on the CPU; the GPU checks resource transport and pixels.
The adapter's FG consumer is captured, so backend-specific texture copying and present hooks
are outside coverage. No model inference or generated-frame quality is claimed. Representative
game runs remain useful for release acceptance and new routes; these deterministic regression
checks do not require opening a game after every change. Wine cannot replace native Windows
D3D12 validation when `DEBUG_LAYER=unavailable`.
