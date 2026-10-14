# Roadmap: what NeuRotic has that this fork does not yet

Started 2026-10-14 from a read-only comparison with NeuRotic (MagicalPrincessUnicorn's DLSS-NR fork, GPL-3; ideas
ported, no code copied). Each item goes the same way: a design note, a step 0 that only logs, a measurement in a game,
and only then the change, as [nr-before-dlssg.md](design/nr-before-dlssg.md) did. Status is updated as items move.

## Done

- **NR ahead of the game's own DLSS-G** ([nr-before-dlssg.md](design/nr-before-dlssg.md)). Measured in Crimson Desert:
  no flicker at 3x, same cost as the upscaled route. Merged in `c9c5e64f`.
- **MFG unlock hardening** (edx wrapper ceiling, transactional gates). Measured: Crimson Desert at 6x (provider
  310.9.1); Stellar Blade's wrapper now passes 5, its provider 310.7.129 still stops at 4x.
- **DRED** ([dred.md](design/dred.md)), **safe deploy** (`tools/deploy-to-game.sh`), **present-guide identity step 0
  measured** ([present-real-guides.md](design/present-real-guides.md)).

## In order

| # | Item | Design | Where it stands | Next step | Needs from a game run |
|---|---|---|---|---|---|
| 1 | Real depth at present with no upscaler | [present-depth-without-upscaler.md](design/present-depth-without-upscaler.md) | design written | step 0b: is depth worth finding at all (scratch build `scratch/depth-0b`, constant depth with real motion, A/B on a held frame) | Crimson Desert, DLAA, `HookMethod=2` |
| 2 | Present route on Vulkan | [nr-vulkan-present-host.md](design/nr-vulkan-present-host.md) | stage 1 (coverage, observe only) built 2026-10-14 on `feat/vulkan-present-host`: `[DlssNr] VulkanPresentHost`, its own hooks also under the marker, owned records, one log line per window; validated under Wine with a native Vulkan application and with DXVK and vkd3d-proton titles; design re-read against today's tree, drift recorded | read the lines in games, then stage 2 (the submission-only control: usage edit at swapchain creation, wait -> unmodified round trip -> signal -> present) | DOOM Eternal and a marked vkd3d-proton title with `VulkanPresentHost=true`: the observe lines |
| 3 | Present route on native D3D11 | [dx11-native-dlss-guides.md](design/dx11-native-dlss-guides.md) | steps 1-2 built 2026-10-13 | the note's next step | a D3D11 title that calls NGX itself |
| 4 | NR before the upscaler (`Stage=1`) by default | [nr-before-upscale.md](design/nr-before-upscale.md) | experimental, built 2026-09-05, never measured here | measure A/B against `Stage=0` | Crimson Desert (RR off), Cyberpunk |
| 5 | MFG on RTX 20/30 | [mfg-rtx20-30.md](design/mfg-rtx20-30.md) | design written 2026-10-14; offline inventory done: every 310.x provider ships its networks as sm_86 SASS, its 31 frame kernels as sm_89/sm_120 only; NeuRotic's is untested on hardware by its own logs | step 0, a log-only probe; then the kernel half forward-compiled on the 4090 | Rafael's RTX 3060 on Windows, `dlssg_for_sm86` removed first |
| 6 | Windows users: package and install path, languages, a route advisor | to write | `tools/package_release.ps1` exists; pt-BR only | design note | a Windows machine |
| - | Native Windows validation | - | DRED and the DLSS-G boundary never ran on Windows | - | Rafael (RTX 3060: DRED, off paths); an RTX 40/50 tester for the boundary |

Deliberately not taken: ten-pass multipass (bounded at three for cost), NeuRotic's NR Anything (Windows Graphics
Capture, not on Proton), its Character Inspector, its machine-translated language packs, its closed Vulkan FG DLL.
