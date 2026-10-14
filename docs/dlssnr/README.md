# DLSS Neural Rendering: design notes

The notes in [design/](design/) are dated records: each carries its own status header, which can be
older than the code. They sit in one flat folder because they link to each other and to
`../../../tests/`. This page groups them by topic. For the code, read
[retoque/dlssnr/README.md](../../retoque/dlssnr/README.md) first (module overview, file index,
shader rebuild). The forwarder DLL is explained in
[retoque/dlssnr/forwarder/README.md](../../retoque/dlssnr/forwarder/README.md), and the evidence
log of the attempt to drop it is [../history/FORWARDER_INVESTIGATION.md](../history/FORWARDER_INVESTIGATION.md).

## Rules and review

- [DEVELOPMENT.md](design/DEVELOPMENT.md): the invariants, state specs and per-change-type review a change must pass before deploy.

## Performance and cost

- [model-cadence.md](design/model-cadence.md): run the model every Nth frame and carry its edit along the motion vectors in between.
- [cadence-smoothing.md](design/cadence-smoothing.md): walk each new answer in over the interval and rate-limit the shown light, against the cadence's pulse.
- [vit-skip.md](design/vit-skip.md): kernel-variant profiling of the model and a proposal to skip its ViT bottleneck on alternating frames.
- [dispatch-cpu-overhead.md](design/dispatch-cpu-overhead.md): persistent mapping of the upload buffers and CBV setup for the composition pass.
- [descriptor-reuse.md](design/descriptor-reuse.md): keeping each ring slot's SRV/UAV descriptors until their key changes.
- [bounded-chain.md](design/bounded-chain.md): the one-to-three-pass D3D12 model chain and its contract.
- [submission-tracking.md](design/submission-tracking.md): submission-ordered retirement of resources used by opt-in D3D12 NR work.
- [working-scale-auto.md](design/working-scale-auto.md): what `WorkingScale=auto` resolves to per GPU, acting on the cross-architecture measurement.
- [reduced-scale-guides.md](design/reduced-scale-guides.md): resampling guides and scaling motion when the model works below the frame's size.
- [peripheral-compression.md](design/peripheral-compression.md): packing the model's input denser in the centre than at the edges.
- [dlssnr-async-compute.md](design/dlssnr-async-compute.md): why the Reshade addon's async compute queue was not ported, and which micro-optimizations were taken instead.

## Measurement

These are records, not designs.

- [gpu-timing.md](design/gpu-timing.md): the contract for fence-confirmed NR GPU timing.
- [detail-measure.md](design/detail-measure.md): the in-game "Measure detail" metric, ported from janblade's fork.
- [pulse-metric.md](design/pulse-metric.md): the light step per frame, model frames against carried ones: model cadence's pulse as a number.
- [nr-log-rate.md](design/nr-log-rate.md): rate-limiting the NR drift logs, from a measured log.
- [stage-before-upscale-measured.md](design/stage-before-upscale-measured.md): running the model before the upscaler, measured in one Cyberpunk 2077 session.
- [model-cost-vs-working-scale.md](design/model-cost-vs-working-scale.md): four GPU measurements of what the pass costs and how it answers to working scale.
- [model-cost-across-architectures.md](design/model-cost-across-architectures.md): the same cost on a card that is not Ada.
- [comparison-capture.md](design/comparison-capture.md): saving the game's frame and the processed frame of the same frame, with provenance, without stalling it (v1 on D3D12: PNG or PFM, a JSON sidecar, `retoque-captures/`).

## Colour, HDR and image quality

- [hdr-highlight-recovery.md](design/hdr-highlight-recovery.md): NVIDIA's highlight-recovery mask applied over the composed frame.
- [hdr-present-conversion.md](design/hdr-present-conversion.md): decoding HDR10 and scRGB to linear BT.709 and re-encoding around the present pass.
- [multi-point-anchoring.md](design/multi-point-anchoring.md): more than one anchor for the scanned-exposure white point.
- [hud-protection.md](design/hud-protection.md): protecting the HUD through the model's UI correction.
- [output-stabilizer.md](design/output-stabilizer.md): hold still pixels still and follow the model where the game moved.
- [residual-limit.md](design/residual-limit.md): an optional flat bound, in stops, on how far the model may move a pixel's light, inside the guard.
- [dlss-enlargement.md](design/dlss-enlargement.md): Transfer 2, the model's edit enlarged by a private DLSS Super Resolution.
- [sharp-residual.md](design/sharp-residual.md): Transfer 3, matched residual with a sharp enlargement.
- [frame-hold.md](design/frame-hold.md): freezing the NR input so settings can be A/B'd on one frame.

## Present and routes

Where the pass runs. Reading order for the present-host family:

1. [nr-without-game-dlss.md](design/nr-without-game-dlss.md): the problem, and where an evaluate could come from when the game never asks.
2. [no-upscaler-titles.md](design/no-upscaler-titles.md): survey of how the rest of the ecosystem handles titles with no upscaler.
3. [nr-present-hook.md](design/nr-present-hook.md): the present-time host, with its reviews and updates.
4. [nr-dx11-bridge-host.md](design/nr-dx11-bridge-host.md): hosting the pass on the D3D11-to-D3D12 bridge, and its lifetime: fullscreen, resizes, a second swapchain on the same window.
5. [nr-vulkan-present-host.md](design/nr-vulkan-present-host.md): one Proton host at Vulkan present.
6. [nr-content-renewal.md](design/nr-content-renewal.md): admission of clean content at present without a present-thread wait.
7. [nr-virtual-backbuffer.md](design/nr-virtual-backbuffer.md): virtual backbuffers on Proton, building on the previous note.

Related, outside that sequence:

- [nr-before-upscale.md](design/nr-before-upscale.md): optional NR before the upscaler (Stage=1).
- [nr-menu-mode.md](design/nr-menu-mode.md): keeping NR running while the upscaler is idle in menus.
- [present-real-guides.md](design/present-real-guides.md): checking that the guides the present pass reuses belong to the frame being presented (design; step 0, log only, built).
- [nr-before-dlssg.md](design/nr-before-dlssg.md): running the present pass at the game's Streamline `Present`, before its DLSS-G interpolates, so generated frames carry the edit; NeuRotic's pre-FG adapter as prior art (design).
- [present-depth-without-upscaler.md](design/present-depth-without-upscaler.md): the game's own depth for the present pass in titles that call no upscaler: heuristic discovery from depth-stencil binds, Streamline depth tags, or synthesized motion alone; NeuRotic's built-in capture as prior art (design; step 0, log only, proposed).
- [dx11-native-dlss-guides.md](design/dx11-native-dlss-guides.md): NR for D3D11 games that call DLSS themselves, through `dlss_12` first (design).
- [pr-improvements.md](design/pr-improvements.md): review of state, exposure, multipass and localization work taken from open PRs.

## Frame generation

- [synthesized-frame-generation.md](design/synthesized-frame-generation.md): frame generation for titles that give no motion, step by step.
- [synthesized-motion.md](design/synthesized-motion.md): synthesized motion vectors for the no-upscaler NR path.
- [mfg-count-override.md](design/mfg-count-override.md): MFG above 2x on Ada under Proton, measured.
- [dlssg-options-via-slsetdata.md](design/dlssg-options-via-slsetdata.md): reaching DLSS-G options through `slSetData` (premise withdrawn, see the note).

## NGX and driver

- [ngx-driver-616.md](design/ngx-driver-616.md): NVIDIA 616.64 and the loader's own route to feature 18.
- [ngx-shutdown-order.md](design/ngx-shutdown-order.md): the D3D12 NGX core shutdown ABI.
- [session-guard.md](design/session-guard.md): keeping NR off for the next launch after a session that ended without closing while NR ran.
- [dred.md](design/dred.md): opt-in D3D12 DRED, so a device removal on native Windows names the queue, the command list and the operation the GPU stopped at.
- [../history/FORWARDER_INVESTIGATION.md](../history/FORWARDER_INVESTIGATION.md) and [retoque/dlssnr/forwarder/README.md](../../retoque/dlssnr/forwarder/README.md) (linked, not design notes).
