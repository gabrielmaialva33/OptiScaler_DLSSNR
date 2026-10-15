# Roadmap: what NeuRotic has that this fork does not yet

Started 2026-10-14 from a read-only comparison with NeuRotic (MagicalPrincessUnicorn's DLSS-NR fork, GPL-3; ideas
ported, no code copied). Each item goes the same way: a design note, a step 0 that only logs, a measurement in a game,
and only then the change, as [nr-before-dlssg.md](design/nr-before-dlssg.md) did. Status is updated as items move.

## Done

- **NR ahead of the game's own DLSS-G** ([nr-before-dlssg.md](design/nr-before-dlssg.md)). Measured in Crimson Desert:
  no flicker at 3x, same cost as the upscaled route. Merged in `c9c5e64f`.
- **MFG unlock hardening** (edx wrapper ceiling, transactional gates). Measured: Crimson Desert at 6x (provider
  310.9.1); Stellar Blade's wrapper now passes 5, its provider 310.7.129 still stops at 4x.
- **The menu under the game's DLSS-G** (D3D overlay): it read translucent at 4x because DLSS-G re-read the frame the
  menu had been drawn into; DLSS-G now pauses while the menu is open on every overlay. Confirmed solid in Crimson
  Desert, with FG resuming on close. The menu blur is off by default (a fork default).
- **DRED** ([dred.md](design/dred.md)), **safe deploy** (`tools/deploy-to-game.sh`), **present-guide identity step 0
  measured** ([present-real-guides.md](design/present-real-guides.md)).

## In order

| # | Item | Design | Where it stands | Next step | Needs from a game run |
|---|---|---|---|---|---|
| 1 | Real depth at present with no upscaler | [present-depth-without-upscaler.md](design/present-depth-without-upscaler.md) | design written; **parked 2026-10-14**: step 0b's A/B (scratch build `scratch/depth-0b`, never merged) was not run, the user declined it, so the present routes keep synthesized motion and constant depth | resume only when someone runs step 0b: is depth worth finding at all (constant depth with real motion, A/B on a held frame) | Crimson Desert, DLAA, `HookMethod=2` |
| 2 | Present route on Vulkan | [nr-vulkan-present-host.md](design/nr-vulkan-present-host.md) | stage 1 (coverage, observe only) and stage 2 (the submission-only control) built 2026-10-14 on `feat/vulkan-present-host`: `[DlssNr] VulkanPresentHost` adds TRANSFER_SRC where the surface offers it and copies every presented image out and back before it is shown -- same queue, or across families from a compute-only presenter -- the application's waits taken over once, a ring of four with fences, queue locks for a borrowed queue, bounded retirement; **stage 2 ran in DOOM Eternal**: 49,143 presents carried, none skipped, 0 ms CPU wait, the Vulkan menu beside it. **Stage 3 (cold NR) built 2026-10-14**: the model runs inside the round trip on SDR swapchains (RGBA8/BGRA8, UNORM or sRGB; HDR refused with a reason) -- a blit into an R16G16B16A16 working colour, encode (passthrough) -> model -> resolve, a blit back -- cold (the driver's NGX core brought up on the transport's device, no game parameters), on zero guides, completion-tracked from the host's fences, one owner of the model in the process (the after-upscale route wins and the host stands down), the model's extensions, `VK_KHR_format_feature_flags2` and bufferDeviceAddress added at vkCreateDevice; present semaphores freed with their swapchain. Under Proton with the real model and synchronization validation: evaluated on every SDR present, cross-family included, read-backs byte-exact with ApplyModel off, about 2.4 ms GPU at 632x446. **Stage 3 ran in DOOM Eternal**: cold NR at present on the start window (pass GPU mean 3.06 ms), the stand-down for the game's DLSS in 12 ms; but with DLSS turned off the route kept the model and the host refused every frame (46 s). Fixed: the host takes the model back once the route has not asked for 10 s (drained behind fences of its own, the route's objects freed, called off if it asks again), held by a 21st wine case (`nr-route-quiet`: a game's upscaler on and off twice) | a second DOOM Eternal run with DLSS toggled (the hand-back's lines, `NR: evaluated` climbing again, `pass GPU` at 3440x1440), then motion for the present hosts on Vulkan (stage 5) and the pacer experiment (stage 4) | DOOM Eternal with `VulkanPresentHost=true`, HDR off: DLSS off, `the model is the present host's again` ten seconds after, `the model runs at present` at 3440x1440; DLSS on (the host stands down), off again (handed back again); a marked vkd3d-proton title, FG off then on, for stage 4 |
| 3 | Present route on native D3D11 | [dx11-native-dlss-guides.md](design/dx11-native-dlss-guides.md) | steps 1-2 built 2026-10-13 | the note's next step | a D3D11 title that calls NGX itself |
| 4 | NR before the upscaler (`Stage=1`) by default | [nr-before-upscale.md](design/nr-before-upscale.md) | experimental, built 2026-09-05, never measured here | measure A/B against `Stage=0` | Crimson Desert (RR off), Cyberpunk |
| 5 | MFG on RTX 20/30 | [mfg-rtx20-30.md](design/mfg-rtx20-30.md) | design written 2026-10-14; offline inventory done: every 310.x provider ships its networks as sm_86 SASS, its 31 frame kernels as sm_89/sm_120 only; NeuRotic's is untested on hardware by its own logs; **step 1 measured**: the 31 frame kernels relabelled sm_86 ran on the 4090 at 4x in Crimson Desert, frames followed the camera (scratch build, never merged) | step 0, a log-only probe for a 30-series card; then step 2, admission only, on Rafael's RTX 3060 | Rafael's RTX 3060 on Windows, `dlssg_for_sm86` removed first |
| 6 | Windows users: package and install path, languages, a route advisor | to write | `tools/package_release.ps1` exists; pt-BR only | design note | a Windows machine |
| - | Native Windows validation | - | DRED and the DLSS-G boundary never ran on Windows | - | Rafael (RTX 3060: DRED, off paths); an RTX 40/50 tester for the boundary |

Deliberately not taken: ten-pass multipass (bounded at three for cost), NeuRotic's NR Anything (Windows Graphics
Capture, not on Proton), its Character Inspector, its machine-translated language packs, its closed Vulkan FG DLL.
