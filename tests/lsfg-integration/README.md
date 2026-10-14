# `lsfg-integration`

Study branch only (`study/lsfg-d3d12`). LSFG 3.1 as FSR-FG's interpolator
(`tools/lsfg-study/OPTI_INTEGRATION.md`, option (b)): the parts of `retoque/lsfg/` that run without D3D12, and
the source guards around the call sites in `OptiScaler/framegen/ffx/FSRFG_Dx12.cpp`.

## What it covers

1. **`LsfgDxil.h`**, the PSV0 reader the root signatures are built from instead of dxcompiler.dll's
   reflection: synthetic DXIL containers with gamma0's real binding layout, record versions 0 and 1 and a
   longer one, part order, no resources; refusals (bad magic, cut header, no DXIL part, no PSV0, unknown
   record size, unbounded or inverted ranges, size and part-count fields past the file); and every truncation
   and single-byte corruption of a valid container under ASan.
2. **`LsfgGraph.h`**, the pass graph the harness validated: 34 + 66 dispatches, dispatch extents at 1920x1080
   against `tools/lsfg-study/PIPELINE_V3_1.md`, `pc_FirstIter`, the UAV/SRV pools, one writer per image and
   no read before its write, the performance and FP16 variants, flow scale sizing only the pyramids, and the
   sizes refused.
3. The constant-buffer table (which of `cb` and the push constants a lone `b0` is) against the CBV counts of
   the converted modules, and the `cb` layout.
4. Which frames LSFG takes (`ClassifySurface`: formats, `_SRGB` and typeless families, HDR10/scRGB, size and
   format mismatches with FFX's output, too small) and the history schedule (warm-up, reset, restart, slots).
4a. The barrier waves `Context_Dx12` records (`ScheduleWaves`): every pass once, program order inside a wave,
   never two writers or a reader and a writer of one image in a wave, every read-after-write, write-after-read
   and write-after-write pair of the sequence in increasing waves; 10 / 36 / 42 waves for the pre-pass, the main
   pass and both, at every size, flow and variant; generate alone in the last wave. And which FFX output formats
   generate may write itself (`DirectOutputFormat`: the working format or its typeless family, never `_SRGB`).
5. `[LSFG]` round trip at all four points: `Config.h` (with defaults), `Config.cpp` read and save,
   `OptiScaler.ini`, `Config.md`; and no fifth `Lsfg*` member outside it.
6. The `FSRFG_Dx12.cpp` call sites (latch, gate, prepare, hand-off, release) as a set, the single FFX
   hand-off, and the disabled path of `BeginFrame`/`TakesFrame` checking `Enabled` before taking any lock.

With `LSFG_DXIL_DIR` pointing at a folder of the user's own converted modules (`Tmp/lsfg-study/dxil`, never
in git), all 99 are parsed and held against the graph the way `ProgramSet_Dx12` and `Context_Dx12` do: CBV
counts, numthreads, sampler spaces, descriptor counts per space, Texture2D against single-layer images.
Without it that part is skipped.

## What it does not cover

D3D12 itself: root signature and PSO creation, descriptor tables, barriers, the recording on FFX's list, the
build worker, parking and the lock (`Lsfg.cpp`, `LsfgFrameGen_Dx12.cpp`). Those were checked in PCSX2 under
vkd3d-proton, which is not a D3D12 validator; native Windows with the debug layer is still open.
`tools/lsfg-study/ctxbench` runs `Context_Dx12` on a real device outside any game and hashes every generated
frame, which is how a change to the recording is shown to leave the images byte for byte as they were.

## Running

```bash
python3 tests/lsfg-integration/run.py
LSFG_DXIL_DIR=/path/to/dxil python3 tests/lsfg-integration/run.py
```
