# Synthesized-motion estimator on a real Vulkan device

Run `python3 tests/synth-motion-vk/run.py` outside a build window (it refuses while `MSBuild`, `build-local.sh` or
`dxc.exe` is running; with other agents on the machine, under `flock -o ~/.cache/retoque-wt/msvc-prefix.lock`).
`--build-only` and `--run-only` split compilation from execution; `--run-only` refuses sources changed since the
build. Output goes to the ignored `artifacts/`: `run.log` (per-frame lines), `run/synth-motion-report.json` (every
measurement) and `result.json` (PASS/FAIL/SKIP with reasons).

## What it runs

The production `SynthMotion::Estimator_Vk`, `VkPipelines` and `VkImages` (`retoque/shaders/synth_motion/SynthMotion_Vk.*`,
the Vulkan twin of the D3D12 estimator: the same FidelityFX optical-flow port and expand pass, compiled to SPIR-V from
the same sources under `VK_MODE`; `docs/dlssnr/design/nr-vulkan-present-host.md`, "Stage 5") with a `pch.h` shim, in an
isolated Wine prefix on winevulkan, which forwards to the machine's driver. The instance asks for Vulkan 1.1, the
estimator's floor, and the device enables `VK_KHR_push_descriptor` alone, as the present host does for the model. No
NGX, no OptiScaler.dll, no game.

It is called the way the Vulkan present host calls it (`DlssNrFeature_Vk.cpp`): the images made and initialised in a
command buffer of their own, submitted and waited for, before the first recording; one ticket per frame;
`ConfirmExecuted` once the frame is submitted, `AbandonRecording` for a frame recorded and never submitted, `Completed`
once its fence is seen. The colour is an RGBA8 UNORM image in `SHADER_READ_ONLY_OPTIMAL`, rewritten from a host buffer
every frame; the field is copied out after the estimator's own closing barrier.

The frames, the sequences and the scores are `tests/synth-motion-d3d12`'s (`../synth-motion-d3d12/sequences.h`), and
`run.py` judges the report with that suite's `judge()` and thresholds, so the two estimators are held to the same
numbers: the uniform pans and each axis' sign, the moving object, the static frame, the cut (a zero field at the cut,
`SceneCut()` within the window, never elsewhere), the abandoned recording (two steps on the next frame), the static
overlay (clear overlay pixels still, smear limits per element, the background held to the small-pan thresholds), and
GPU time at 1920x1080 and 3440x1440. The `hud_*` sequences are the D3D12 suite's HUD mask's and do not run here.

On top of that judgement:

- the validation layer (core and synchronization) ran: a deliberately invalid fence flag must reach the application's
  messenger with its VUID and be refused;
- it reported no error over the whole run;
- every field is at the colour's extent.

`SceneCut()` is raised one frame after the cut here (it is read when the cut frame's fence is seen), three on D3D12
(read from a slot old enough to have landed). Both are inside the judged window.

GPU time is the estimator's recording bracketed by timestamps, reported and never judged, with clocks unlocked.
Measured on the RTX 4090 (driver 615.78.08), 2026-10-15: 0.80 ms at 1920x1080 and 1.79 ms at 3440x1440; the D3D12
estimator's suite, run the same hour with clocks unlocked, measured 0.86 ms and 2.63 ms.

## SKIP

Exit 0, `"status": "SKIP"` in `result.json`, when `SynthMotion::VkUnsupported` refuses the device (an instance or device
below Vulkan 1.1, no subgroup arithmetic in compute, subgroups other than 32 or 64 wide, fewer than nine push
descriptors, a format without the storage the estimator needs). The reason is in the report.

## Limits

Synthetic content, integer motion, SDR RGBA8 input, one device and queue, frames never overlapping on the GPU. It says
the Vulkan estimator measures what the D3D12 one does and that its commands are valid and synchronised; what the model
makes of the field is `tests/vulkan-present-host`'s `nr-synth-motion` case, and a game's.
