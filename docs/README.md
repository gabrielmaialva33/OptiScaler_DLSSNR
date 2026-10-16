# Documentation index

This fork (Retoque, `OptiScaler_DLSSNR` until 2026-10-13) keeps upstream OptiScaler's own documents at the
repository root, where upstream has them: `README.md`, `Features.md`, `Spoofing.md`, `Issues.md`,
`Changelog.md`, `CONTRIBUTING.md` and the INI reference `Config.md`. Everything this fork added lives
here or beside the code it describes.

## Users

- [Config.md](../Config.md): every INI key, including the `[DlssNr]` block.
- [README.md, Installation](../README.md#installation): Linux and Windows; on Windows the release's `install-windows.cmd`, described in [tools/README.md](../tools/README.md#install-windowsps1-and-install-windowscmd).
- [guides/ExternalDlssgTelemetry.md](guides/ExternalDlssgTelemetry.md): feeding DLSSG source-frame telemetry from an external DLL/ASI.

## Linux and Proton

- [linux-proton/vulkan-overlay-nongraphics-present.md](linux-proton/vulkan-overlay-nongraphics-present.md): decision record for the Vulkan overlay on a non-graphics present family.
- [CLAUDE.md](../CLAUDE.md): "Linux / Proton patches" lists the fork's overlay patches over upstream.

- [linux-proton/fork-fixes-20261016.md](linux-proton/fork-fixes-20261016.md): NGX/library compatibility, menu GPU completion and FG lock-pairing fixes from the fork audit.
- [linux-proton/streamline-game-camera.md](linux-proton/streamline-game-camera.md): preserve valid game camera matrices by frame and D3D12 resource-tag origins.

## DLSS Neural Rendering design

- [dlssnr/README.md](dlssnr/README.md): topic index of the design notes in [dlssnr/design/](dlssnr/design/), with a reading order for the present hosts.
- [dlssnr/design/DEVELOPMENT.md](dlssnr/design/DEVELOPMENT.md): the invariants and per-change review every NR change is held to.

## History

- [history/FORWARDER_INVESTIGATION.md](history/FORWARDER_INVESTIGATION.md): evidence log of the attempt to drop the forwarder DLL, superseded in part by `ngx-driver-616.md`.

## Tests

- [tests/README.md](../tests/README.md): suite index, tiers and what each suite does and does not cover.

## Code-adjacent READMEs

- [retoque/dlssnr/README.md](../retoque/dlssnr/README.md): NR module overview, file index, shader rebuild.
- [retoque/dlssnr/forwarder/README.md](../retoque/dlssnr/forwarder/README.md): why `nvngx.dll_dlssnr.dll` exists.
- [retoque/lsfg/README.md](../retoque/lsfg/README.md): LSFG 3.1 D3D12 pipeline and custom MFG presenter (x2/x3/x4).
- [retoque/shaders/synth_motion/README.md](../retoque/shaders/synth_motion/README.md): the synthesized-motion estimator and the provenance of its FidelityFX-derived files.
- [retoque/translations/README.md](../retoque/translations/README.md): the optional pt-BR text pack (shipped in the release zip).

## Tools and Studies

- [tools/README.md](../tools/README.md): build, deployment, the Windows installer, release packaging and log summarizer scripts.
- [tools/lsfg-study/README.md](../tools/lsfg-study/README.md): LSFG on D3D12 study, bytecode conversion tooling and integration notes.

## Licences

- [Licenses/](../Licenses/): attribution and licence texts that ship with a distributed build.
