# DLSS-NR: asynchronous compute queue — analysis and decision

Scope: whether the asynchronous compute design of the Reshade-based DLSS5 addon
(`deps/DLSS5-Reshade-AIO`) can or should be ported into OptiScaler's DLSS-NR
(`nvngx_dlssnr.dll` path) to reduce input latency or improve performance.

Decision: **not ported.** The two projects sit at different positions in the
render stack, and the mechanism that makes the async queue valuable in the
addon — presenting the addon's own output — does not exist here. The
micro-optimizations that *do* transfer were taken instead (commits referenced
below).

## Where each project runs the model

**OptiScaler.** `DlssNr_Dx12::EvaluateAfterUpscale` is called from the
upscaler's `EvaluateAfterUpscale` hook, i.e. on the *game's own* D3D12 command
list, immediately after the game's upscaler (DLSS SS) has written its output.
`Dispatch` records three `DispatchPass` compute dispatches (encode, downsample,
resolve) and the NGX feature evaluation onto that same list, reading and writing
the game's own buffers in place. The game then presents its own backbuffer
through its own swapchain, which OptiScaler wraps (`wrapped_swapchain.cpp`)
but does not replace.

**DLSS5-Reshade-AIO.** The addon is a full renderer. It captures the game's
backbuffer into private per-frame slot textures
(`g_pipeline_slots[]`, `PipelineFrameSlot`), runs everything — motion
analysis, NR, DLSS, FG — on queues of its own, and presents the result through
its *own* DComposition proxy swapchain and window (`g_proxy_swapchain`,
`g_proxy_window`, `g_composition_target` in `addon/src/nr-standalone.cpp`).
The game's swapchain is bypassed for the final image.

That difference is the whole story. Everything the addon does asynchronously
exists to feed a presentation that the addon controls.

## How the addon's async pipeline works

- `g_async_compute_queue`: a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue on a
  second D3D12 device (`g_neural_device`) with its own fence (`g_neural_fence`).
  A separate `g_async_fg_queue` runs frame generation.
- Cross-queue dependency (`QueueAsyncInputDependency`): the queue that owns the
  captured backbuffer signals `g_async_input_fence`, the compute queue waits on
  the same fence value. This makes the async list see the capture without CPU
  serialization.
- A per-frame slot ring with an explicit state machine (free → capturing →
  ready → in-flight → presented), GPU telemetry consumed per slot
  (`ConsumeGpuTelemetry`), and a bounded three-buffer
  frame-latency-waitable proxy swapchain driven by a dedicated worker thread
  (v1.7.22 release notes).
- NVOF (driver `nvofapi64.dll` hardware optical flow) runs on the async queue
  with VORT / zero-motion fallbacks.

## Why it does not transfer

1. **There is no OptiScaler output to present asynchronously.** To run the
   model off the game's queue, OptiScaler would have to: copy the upscaler
   output into a private resource, cross the fence, run the model, copy the
   result back into the game's buffer, and make the wrapped `Present` wait on
   a completion fence before the real present. Each step is individually
   feasible — OptiScaler already intercepts present — but every frame pays
   two extra full-resolution copies, and the failure mode of a fence mistake
   inside an injected game hook is a hang or TDR, not a dropped frame.

2. **The net GPU work does not decrease; it only overlaps.** On the game's
   queue the model is serialized after the upscaler; on an async queue it runs
   *alongside* whatever else is on the game's queue after the upscaler (FG
   synthesis, post effects). That overlap buys time only when the game's queue
   has idle time after the upscaler. In a GPU-bound 4K scene (the common KCD2
   case) the GPU has no spare capacity, the copies are pure extra bandwidth,
   and total time to present is the same or worse.

3. **The addon's own documentation concedes the latency cost.** Its README
   states that "the asynchronous pipeline can retain one or more captured
   frames while motion analysis, NR, DLSS, FG, and presentation complete; at a
   30 FPS source cadence, every additional queued frame is about 33 ms old",
   and that "controls feel noticeably delayed" is a known symptom with a
   per-game opt-out. The mechanism trades freshness for throughput; it is not
   a latency reduction.

4. **Input latency is not where this mechanism acts.** The input→photon chain
   is: game input poll → the frame that input affects → render → FG (one frame
   by design when enabled) → present. Moving the model off the game's queue
   does not shorten any of those links; it changes where one GPU-bound segment
   executes, which per (2) and (3) is neutral to negative for this workload.

## What was transferred instead

The per-frame CPU cost on the game's thread, which *is* a real latency
contributor on CPU-bound or frame-pacing-limited machines:

- `7c62de72` — the exposure-sweep light probe drops to a liveness cadence
  (every 120 frames) once it has proven the scene is static, instead of
  re-scanning every 4th frame.
- `708f1dd8` — `DispatchPass` no longer re-creates its five SRVs, two UAVs and
  the 256-byte constants buffer when the bindings it describes have not moved
  (a full-texture view is a pure function of its resource). Steady-state
  per-dispatch CPU setup is near zero; first use and every resize/recreate
  still write everything.
- `13cb1bce` — the GPU-timestamp readbacks (three readback heaps Map/Unmap'd
  on the game thread per present: upscaler/RCAS/output, their FG-path
  equivalents, and the NR total/model pair) run only while the performance
  display is on screen, since that display is their only consumer. The
  timestamp queries keep recording; nothing else reads the values.

## Revisit conditions

The async design is worth revisiting only if profiling the target game shows
the game's command queue idle after the upscaler completes — i.e. a
CPU-bound or scene-light configuration where the model would otherwise sit in
queue headroom. The concrete shape would then be the five-step flow in
(1) above, with a completion-fence wait in the wrapped present, a two-copy
budget, and a per-game opt-out. Until that condition is measured, the
serialized in-place path is the lower-risk, lower-overhead choice.
