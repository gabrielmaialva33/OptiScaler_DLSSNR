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
| `hud_+8x`, `hud_+3+2` | the same overlay plus a 1 px hollow frame with a drop shadow and a translucent panel, over pans of (+8, 0) and (+3, +2), 40 frames | the HUD mask and the UI layer's own mask only (below) |
| `time_1080p`, `time_3440` | 32-frame pans at 1920×1080 and 3440×1440 with the `hud_*` overlay | GPU time only |

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

## Synthesized FG's HUD mask

Every frame of every sequence also runs `SynthMotion::Overlay_Dx12` (`shaders/synth_motion/SynthOverlay_Dx12.*`,
the HUD mask of "The HUD: near depth and a UI layer" in `synthesized-frame-generation.md`), on the same
list, right after the estimator and on the same frame. On `overlay_*` it runs the mask alone, the default
(`SynthesizedHudDepth` without the layer); everywhere else it also computes the UI layer's own mask and records the
layer from that frame ("The HUD layer's own mask: recall and a margin"). The depth, the mask, and with the layer
its core and the layer itself are read back and scored. Timestamps 2 to 3 bracket the mask pass, 3 to 4 the layer
pass.

- **Exact, everywhere:** the depth is 0 or 1 and agrees with the mask. With the layer: its rgb is the frame's own,
  byte for byte (an RGBA8 frame gives an RGBA8 layer); its alpha is the read-back core dilated and feathered on the
  CPU at the default margin (`kLayerMargin`, held equal to `[FrameGen] SynthesizedHudMargin`'s default), within one
  step, and never under the strict mask; the core is never under the strict mask.
- **No scenery:** on the pans, the moving object, static, cut and abandon, not one pixel is marked, and not one
  pixel of the layer's core or alpha.
- **Precision, on `overlay_*` and `hud_*`, from t=10** (the rule's 8-frame entry streak starts at t=1): at
  least 99% of the marked pixels are overlay pixels or within 1 px of one. The same for the layer's core on `hud_*`.
- **Recall, on `hud_*`**, the same overlay over 40 frames, estimator not scored, mean from t=30:
  - the strict mask: at least 10% of the crosshair's pixels and 15% of the floating text's. These are floors under
    what was measured on 2026-09-28 (34%/39% at 8 px a frame, 21%/30% at (3, 2)). About half of each element is
    its 1 px dark outline, which the rule never marks, and the protection builds up as the scene moves past. The
    panel, the frame and the glass are reported only: the strict rule marks none of them;
  - the layer's core: at least 95% of the crosshair, text, frame and panel, and 85% of the glass's opaque parts (its
    fill is the scene behind, darkened, and never still). Measured 100% and 93% on 2026-09-29.
- **The band, on `hud_*`**, reported against the old layer (whose alpha was the strict mask):
  - every overlay pixel is one part (stroke, outline or fill), and the strict mask, the core and the alpha are
    reported per element and part;
  - the background's mean alpha in rings 1-2, 3-4, 5-8, 9-16, 17-24 and 25+ px from the nearest overlay pixel;
  - the smear set: the background whose sample at half the pan (q + d/2 or q - d/2, both roundings) lands on the
    overlay, which is where FSR blends the overlay in under flat depth. Its mean alpha is judged: at least 90% of
    all of it, 95% beside the crosshair, text, frame and panel, and 60% beside the glass. The glass's 1 px border
    has no still neighbour to contrast with (the scene outside and its own fill inside both move), so it seeds only
    near the glass's text and grows 48 px along itself from there: its bare bottom edge stays out;
  - the held background: the alpha outside the smear set, base-frame pixels where interpolation was right;
  - a sweep of margins 0 to 24, dilated on the CPU from the core.
- **GPU time** on the timing sequences, whose first half runs the mask alone and second half the layer: the mask
  alone, and with the layer the mask pass and the layer pass. Reported, never judged.

`SYNTH_HUD_DUMP=<dir>` (a Windows path, `Z:\...` for a Linux one) writes each sequence's last layer core and alpha
as PGM files there, for looking at.

None of this runs FSR, so it says the masks are right on synthetic frames, not what FSR-FG does with them.

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
