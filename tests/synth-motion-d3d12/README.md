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
| `overlay_+8x`, `overlay_+8+8` | a static crosshair, HUD panel and outlined glyphs over a background panning (+8, 0) and (+8, +8) | clear overlay pixels against zero; the background outside a 24 px band around each element against the pan |
| `time_1080p`, `time_3440` | 24-frame pans at 1920×1080 and 3440×1440 | GPU time only |

GPU time is `Record` bracketed by timestamps on the same list. It is reported, never judged, and
labelled with the clock state: `sudo -n nvidia-smi -lgc 2100,2100 -lmc 10501` if available (reset
afterwards), otherwise "UNLOCKED".

Only frames where `Ready()` is true are scored: the estimator yields no field for FFX's five warm-up
frames after a reset. Thresholds and their reasons are in the comment block at the top of `run.py`.

## The overlay sequences

They reproduce Generation Zero's smeared crosshair and HUD under synthesized FG (2026-09-28), which
the expand pass's per-pixel choice fixes ("Static overlays" in
`OptiScaler/dlssnr/design/synthesized-motion.md`). Every colour pixel falls in one class:

- **Overlay, clear.** An overlay pixel the frame pair can show is static. Judged: a mean `|v|` under
  0.5 px.
- **Overlay, ambiguous.** An overlay pixel whose 3x3 in frame t is reproduced exactly by frame t-1
  displaced by the camera's motion: a flat fill, or a stroke along the motion. No estimator that sees
  only the pair can tell it from moving content, and displacing it reads the same overlay. Not judged;
  it is in the `overlay` total the report prints.
- **Overlay, smeared** (any overlay pixel, clear or not). A vector over 0.5 px whose displacement
  reads another colour than the overlay's: what frame generation drags. Judged: at most 2% of all
  overlay pixels, and 5% of each element's (crosshair, panel, floating text). The share with
  `|v| > 0.5` px is reported and not judged. A vector blended across an element's edge can displace
  a stroke or a fill along itself; the pair cannot object to that, and it reads the same overlay.
- **Background.** Outside the border band and a 24 px band around each element. Held to the
  small-pan thresholds.
- **Band.** The 24 px around each element: block vectors blended across its edge, and the background
  uncovered from behind it. Reported, not judged.

The report prints every class per sequence, with each element's clear pixels (crosshair, panel,
floating text) apart. Under `--source nvofa` the overlay is reported, not judged: that source has no
per-pixel choice. Measured before and after the change: "Static overlays", "Measured" in the design
note.

## `--source nvofa`

`--source nvofa` runs the same binary and sequences through `SynthMotion::NvofaEstimator_Dx12`, the
NVIDIA Optical Flow source. The Proton's own dxvk-nvapi `nvofapi64.dll` is copied beside the harness,
preferably from the Proton that supplied vkd3d-proton. Its artifacts carry a `-nvofa` suffix
(`run-nvofa.log`, `run/synth-motion-report-nvofa.json`, `result-nvofa.json`).

- **Lag.** The field is one frame late by contract, so frame t is scored against the truth of t-1.
  That is the same for the pans and one frame behind for the object; the abandon's two-step frame is
  t=10.
- **Scene cut.** The source has none. `SceneCut()` must never be raised, the pair spanning the cut is
  not scored, and frames from t=11 on must be as accurate as a small pan.
- **GPU time** covers the two shader passes on the list, not the engine, which runs on its own queue.
- **SKIP** (exit 0, `"status": "SKIP"` in `result-nvofa.json`) when:
  - the NVOFA shaders are not generated;
  - no Proton ships `nvofapi64.dll`;
  - the estimator reports itself unavailable on the device (missing `VK_NV_optical_flow`, a refused
    format, and so on). The reason is in the report.

`run_all.py` runs the default source only.

## Limits

Synthetic content, integer motion, SDR RGBA8 input, one device and queue, frames never overlap on
the GPU. It says the estimator is correct and how fast it is on this machine; it says nothing about
how the NR model or FSR-FG respond to its field. That needs a game.
