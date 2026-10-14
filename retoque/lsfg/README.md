# Lossless Scaling Frame Generation on D3D12 (`retoque/lsfg`)

A self-contained module that executes Lossless Scaling's frame generation algorithm (LSFG 3.1)
natively on D3D12 inside OptiScaler, integrated as the interpolation backend for frame generation
or presented through Retoque's custom multi-frame generation (MFG) swapchain at x2, x3, and x4.

LSFG shaders and models originate from the user's licensed Lossless Scaling installation (`lsfg-vk.dll`)
and are not redistributed in this repository.

## Architecture

The integration consists of three primary layers:

1. **Pass Graph & DXIL Dispatch (`retoque/lsfg/`)**:
   - Reconstructs LSFG 3.1's 99 compute passes in Direct3D 12.
   - Schedules barrier waves (`LsfgGraph.h` `ScheduleWaves`), reducing hundreds of individual resource
     barriers into batched waves of mutually independent dispatches.
   - Resolves root signatures directly from the DXIL container's PSV0 part (`LsfgDxil.h`) without
     requiring `dxcompiler.dll` at runtime.
   - Direct UAV output (`outputs[0]`) avoids intermediate copy passes when format and access flags allow.

2. **Presenter & Multi-Frame Generation (`retoque/lsfg/presenter/`)**:
   - Custom D3D12 frame-generation swapchain (`MfgSwapChain_Dx12`) managing frame pacing, presentation timing,
     and multi-frame interpolation (x2, x3, and x4).
   - Dedicated UI composition pass (`MfgSwapChain_UiComposition`) protecting in-game HUDs and overlays from
     interpolation smearing.
   - Diagnostic pacing tracker (`MfgSwapChain_DebugPacing`) reporting per-frame latency deltas.

3. **Menu & UI (`LsfgMenu.{h,cpp}`)**:
   - ImGui controls in the OptiScaler overlay for toggling LSFG, setting multipliers (x2, x3, x4),
     flow scale, and performance mode.

## Files

| File | Role |
|---|---|
| `Lsfg.h`, `Lsfg.cpp` | Module lifecycle, feature detection, and hook handoff. |
| `LsfgFrameGen_Dx12.h`, `LsfgFrameGen_Dx12.cpp` | D3D12 frame-generation pipeline and resource binding. |
| `LsfgGraph.h` | Pass graph execution, resource state tracking, and barrier wave scheduling. |
| `LsfgDxil.h` | DXIL container / PSV0 reflection for runtime root signature generation. |
| `LsfgShaderSource.h`, `LsfgShaderSource.cpp` | Shader loader abstraction (`IShaderSource`). |
| `LsfgShaderLibrary.h`, `LsfgShaderLibrary.cpp` | Shader catalog and SPIR-V/DXIL bytecode bridge. |
| `LsfgEnvironment.h` | Environmental knobs and configuration flags. |
| `LsfgMenu.h`, `LsfgMenu.cpp` | OptiScaler in-game overlay menu tab and real-time telemetry. |
| `presenter/LsfgPresenter.{h,cpp}` | Multiplier orchestrator and presentation driver. |
| `presenter/MfgSwapChain_Dx12.{h,cpp}` | D3D12 swapchain wrapper for pacing and generated frame presentation. |
| `presenter/MfgSwapChain_UiComposition.{h,cpp}` | UI layer blending and HUD protection passes. |
| `presenter/MfgPacing.h`, `presenter/MfgLatch.h` | Timing calculations, queue latching, and latency bounds. |
| `presenter/MfgSwapChain_Helpers.{h,cpp}` | Command list and DXGI swapchain utilities. |

## Studies and Design Documentation

Detailed research, contracts, and reverse-engineering findings are documented in [`tools/lsfg-study/`](../../tools/lsfg-study/):

- [`tools/lsfg-study/README.md`](../../tools/lsfg-study/README.md): Tooling overview, SPIR-V extraction, and bytecode translation.
- [`tools/lsfg-study/OPTI_INTEGRATION.md`](../../tools/lsfg-study/OPTI_INTEGRATION.md): Integration design decisions, barrier waves, and UAV routing.
- [`tools/lsfg-study/MFG_CONTRACT.md`](../../tools/lsfg-study/MFG_CONTRACT.md): The contract between the frame interpolator and the presenter.
- [`tools/lsfg-study/MFG_PRESENTER.md`](../../tools/lsfg-study/MFG_PRESENTER.md): Custom swapchain design, frame queueing, and pacing for x2/x3/x4.
- [`tools/lsfg-study/PIPELINE_V3_1.md`](../../tools/lsfg-study/PIPELINE_V3_1.md): Pass order, pyramid dependencies, and resource bindings of LSFG 3.1.
- [`tools/lsfg-study/FP16_NOTES.md`](../../tools/lsfg-study/FP16_NOTES.md): Precision benchmarks (FP16 vs FP32) and performance measurements.
