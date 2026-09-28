# Synthesized-motion estimator on a real D3D12 device

Run `python3 tests/synth-motion-d3d12/run.py` outside a build window (it refuses while `MSBuild`,
`build-local.sh` or `dxc.exe` is running). `--build-only` and `--run-only` split compilation from
execution; `--no-lock` skips the clock lock. Output goes to the ignored `artifacts/`:
`run.log` (per-frame lines), `run/synth-motion-report.json` (every measurement) and
`result.json` (PASS/FAIL with reasons).

## What it runs

The production `SynthMotion::Estimator_Dx12` (`OptiScaler/shaders/synth_motion/`, the FidelityFX
optical flow port, see `OptiScaler/dlssnr/design/synthesized-frame-generation.md`) compiled with a
`pch.h`/`Logger.h` shim, on vkd3d-proton in an isolated Wine prefix. The compiler prefix is a
private copy of the local MSVC prefix. No NGX, no OptiScaler.dll, no game.

Each frame the harness writes a synthetic RGBA8 frame into a texture, calls `Record` on its own
list, copies `Motion()` to a readback buffer, executes, waits, calls `ConfirmExecuted()` and scores
the field. Content is an integer-sampled procedural pattern (value noise at 3/9/27/81 px over a
brick grid), so an integer shift is an exact translation and the ground truth needs no
interpolation.

Convention under test: `Motion()` is the displacement from the current frame to the previous one,
in colour pixels, +x right, +y down. Content moving `(+dx, +dy)` per frame reads `(-dx, -dy)`.
The pans with a single non-zero axis pin each axis' sign independently.

| sequence | content | scored against |
|---|---|---|
| `pan_*` (8) | uniform pans of 1, 4, 16 and 48 px in several directions, 1280×720, 14 frames | interior away from a border band of `32 + 2·|d|` |
| `object` | 256×192 patch moving (+8, +3) over a static background | patch interior (24 px inset) and background outside a 48 px band around both positions |
| `static` | the same frame every time | zero |
| `cut` | pan, then at t=8 different and much darker content, 18 frames | a zero field at t=8; `SceneCut()` within t=8..12 (it is read back from the GPU, `ReadbackSlots` late at most); steady frames scored as a pan |
| `abandon` | pan (+4, 0); t=8 recorded and thrown away via `AbandonRecording()` | t=9 must read two steps (−8), later frames one step |
| `time_1080p`, `time_3440` | 24-frame pans at 1920×1080 and 3440×1440 | GPU time only |

GPU time is `Record` bracketed by timestamps on the same list. It is reported, never judged, and
labelled with the clock state: `sudo -n nvidia-smi -lgc 2100,2100 -lmc 10501` if available (reset
afterwards), otherwise "UNLOCKED".

Only frames where `Ready()` is true are scored: the estimator yields no field for FFX's five warm-up
frames after a reset. Thresholds and their reasons are in the comment block at the top of `run.py`.

## Limits

Synthetic content, integer motion, SDR RGBA8 input, one device and queue, frames never overlap on
the GPU. It says the estimator is correct and how fast it is on this machine; it says nothing about
how the NR model or FSR-FG respond to its field. That needs a game.
