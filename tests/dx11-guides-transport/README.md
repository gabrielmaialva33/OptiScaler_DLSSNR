# Native D3D11 guide transport under Proton (wine tier)

```bash
python3 tests/dx11-guides-transport/run.py                 # the transport's sequence
python3 tests/dx11-guides-transport/run.py --check-d3d11   # the required cases with a D3D11 read-back before each D3D12 read
python3 tests/run_all.py --tier wine --only dx11-guides-transport -v
```

Step 3 of `docs/dlssnr/design/dx11-native-dlss-guides.md` carries a native D3D11 DLSS evaluate's depth and motion
to the D3D11 bridge's D3D12 device. This suite runs that sequence on real devices, under a local Proton (PROTON_DIR,
else the newest GE-Proton, else Valve's), so the D3D11 side is DXVK and the D3D12 side vkd3d-proton, as in a game.
The harness is built with MSVC in a private copy of the msvc-wine prefix; nothing of OptiScaler is loaded.

For each case, a game-like texture is made once, with a shared copy, its D3D12 opening, and a clone made the way
the present guide ring makes its clones (`CreatePresentGuideClone`). Then six frames: D3D11 writes new contents
(a depth clear, or a pattern no other frame and no stale copy can match), copies them into the shared texture,
signals a shared fence and flushes; the D3D12 queue waits on that fence and runs a list with `CaptureGuides`'s
barriers and copy (the shared texture COMMON -> COPY_SOURCE -> COMMON), and the clone is read back and compared.

What it says, as of 2026-10-15 under GE-Proton11-7 on an RTX 4090:

- **Everything the transport sends crosses, every frame**: R32_TYPELESS and R16_TYPELESS depth-stencils through the
  transport's own shader-resource-only copy (legacy and NT handles), an R32_FLOAT depth render target, and R16G16,
  R32G32 and R16G16B16A16 float motion through upstream's copy path.
- **The hazard the own copy exists for**: upstream's copy of a depth-stencil keeps `D3D11_BIND_DEPTH_STENCIL`, and
  D3D12 never sees D3D11's copy into it. The case runs in four variants, each logging every frame's first texel as
  D3D12 read it against what D3D11 wrote:

  | D3D11 between its copy and the D3D12 read | Crossed | D3D12 read |
  |---|---|---|
  | shared fence only (the transport's order) | 0 of 6 | zero, every frame |
  | fence, then a CPU wait for D3D11's work (event query), no access | 0 of 6 | zero, every frame |
  | a D3D11 read of the shared texture before the signal, no wait | 6 of 6 | each frame's own value |
  | fence, then a D3D11 read-back of the shared texture (access and CPU wait) | 6 of 6 | each frame's own value |

  Not ordering and not a lag: no frame read an earlier frame's value, and a CPU wait alone changes nothing. D3D12 sees
  the copy only once D3D11 has accessed the shared image after writing it, which points at the state DXVK leaves a
  depth-stencil image in after a copy (layout or compression at the hand-off). The case is reported as a hazard and
  not required, so a runtime that fixes it shows "every frame crossed" on its fence-only line instead.
  `--check-d3d11` runs the required cases in the read-back variant.

What it does not cover: upstream's `DepthTransfer_Dx11` pass for typeless depth-stencil formats with a stencil plane
(R24G8, R32G8X24), which it does not run, nor the state the transport saves and restores around it; the bridge's own
statics and shared fence (the harness makes the same calls with objects of its own); sets overlapping in flight (it
waits for each frame on the CPU and flushes after the signal, as a game's present would); native Windows, where the
D3D12 debug layer is the check; and anything about the picture.
