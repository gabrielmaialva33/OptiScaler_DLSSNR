# Retoque

**DLSS 5 Neural Rendering for any game, on Linux and Windows. Built on OptiScaler.**

Retoque runs NVIDIA's DLSS 5 Neural Rendering model over a game's picture. It is a fork of
[OptiScaler](https://github.com/optiscaler/OptiScaler), by way of
[Dagherbou's DLSS-NR fork](https://github.com/Dagherbou/OptiScaler_DLSSNR), and like OptiScaler it loads
as a proxy DLL between the game and the graphics API, so the game needs no patch and no cooperation.
Everything OptiScaler does (swapping upscalers, frame generation, spoofing) is still here; see
[OptiScaler's README](https://github.com/optiscaler/OptiScaler#readme) for that side.

> Retoque does **not** include the Neural Rendering model. You supply `nvngx_dlssnr.dll` yourself (see
> [Requirements](#requirements)).

## What it does

The model runs on whichever route the game allows:

- **After the game's upscaler, on D3D12.** The usual route: the model works on the upscaler's output,
  before the game draws its HUD and before frame generation. FSR 2/3 and XeSS games work too, because
  OptiScaler normalises their calls into the same path.
- **On Vulkan.** Native Vulkan games; DOOM Eternal is the reference.
- **On D3D11 games, through a D3D11-to-D3D12 bridge.** Divinity: Original Sin 2 and The Witcher 3 (DX11
  renderer) run this way, with no upscaler in the game at all.
- **At present time, for games with no upscaler.** The pass runs on the finished backbuffer, which
  covers emulators such as PCSX2. With no depth or motion from the game it can estimate motion from the
  frames themselves (`SynthMotion`, experimental).

Also in the tree:

- **Linux / Proton fixes:** Vulkan hook skipping, overlay routing, and the vkd3d-proton / DLSS-G
  interlock.
- **Frame generation:** the pass runs before frame generation sees the frame, and frame generation can
  be synthesized for games that supply no motion vectors.
- **Colour:** RenoDX-style colour composition, a white point taken from the game's exposure, and
  optional highlight recovery. HDR10/PQ and scRGB swapchains are declined at present time for now (the
  game's picture is shown untouched) until that conversion exists.
- **Controls:** working scale, peripheral compression, output stabilizer, model cadence, side-by-side
  and wipe comparison, all from an in-game menu.
- **Measurement:** fence-confirmed GPU timing, per-kernel profiling and an in-game "Measure detail"
  tool.
- **A host test suite** that runs without a GPU (see [Tests](#tests)).

At strength zero the frame is bit-identical to what the game produced, and with Neural Rendering off
the module does nothing.

## Status

Experimental, and unsupported by NVIDIA. Measured working on Linux (Proton, RTX 4090):

| Game | Route |
|---|---|
| Crimson Desert | after the upscaler (D3D12), with DLSS-G |
| Divinity: Original Sin 2 | D3D11 bridge, with synthesized frame generation |
| The Witcher 3 (DirectX 11) | D3D11 bridge |
| DOOM Eternal | Vulkan, plus the menu overlay |
| PCSX2 | D3D12 present pass |

Builds are also run on native Windows with an RTX 3060. Proton is not a D3D12 validator (it accepts
calls that Windows rejects), so Windows reports are especially welcome.

## Requirements

- An **NVIDIA RTX GPU**. The cost depends a lot on the architecture: `WorkingScale=auto` runs the model
  at full scale on RTX 40 and later, and at half scale on RTX 20 and 30.
- **`nvngx_dlssnr.dll`**, the Neural Rendering model, which you supply. The 310.8.2.0 build in common
  use is a community patch of NVIDIA's 310.8.0.0 (NVIDIA's own build is known to fault). Retoque does
  not redistribute either one.
- **`nvngx.dll_dlssnr.dll`**, a small forwarder DLL built by this project. The model refuses callers whose
  module path lacks `nvngx.dll`, so its calls go through this file. It contains no NVIDIA code.
- A recent NVIDIA driver. Development runs on the Linux 615.78 driver; there is no tested minimum yet.
  On Proton, copy `/usr/lib/nvidia/wine/_nvngx.dll` into the game folder and refresh it when the driver
  updates.

## Install on Linux (Proton)

1. Copy `OptiScaler.dll`, `OptiScaler.ini`, `nvngx.dll_dlssnr.dll` and your `nvngx_dlssnr.dll` into the
   folder that holds the game's executable.
2. Rename `OptiScaler.dll` to a name the game loads early: `dxgi.dll`, `winmm.dll`, `version.dll`,
   `dbghelp.dll`, `d3d12.dll`, `wininet.dll` or `winhttp.dll` (or `OptiScaler.asi`). `setup_linux.sh`
   asks and renames it for you.
3. Steam launch options, for a `dxgi.dll` proxy: `WINEDLLOVERRIDES="dxgi=n,b" %command%`
4. In `OptiScaler.ini`, change the existing keys in place. Do not add a second copy of a key: the
   parser keeps the last one.
   ```ini
   [DlssNr]
   Enabled=true
   ToggleKey=0x76    ; F7 switches the pass on and off (no key by default)
   ```
   The in-game menu opens with **Insert** (`[Menu] ShortcutKey`).
5. Native Vulkan games under Proton can hit fence timeouts when the Vulkan overlay runs next to DLSS-G.
   Set `[Vulkan] SkipHooks=true` for those.

Keep **one** proxy name per game folder: two copies loaded together fight over the same hooks and the
game dies at startup. The log line `CheckWorkingMode OptiScaler working as <name>` says which one
loaded. If the game's prefix has no `nvapi64.dll` (Divinity is one), point `[Libraries] NvapiPath` at
Proton's own `files/lib/wine/nvapi/x86_64-windows/nvapi64.dll`.

## Install on Windows

The same files and the same ini changes, without `WINEDLLOVERRIDES`: copy them beside the game's
executable and rename `OptiScaler.dll` to the proxy name. `setup_windows.bat` does the rename.

## Key settings

Every key lives under `[DlssNr]` in `OptiScaler.ini` and ships as `auto`; the table says what `auto`
means. [`Config.md`](Config.md) and the comments in the ini are the full reference.

| Key | `auto` means | What it does |
|---|---|---|
| `Enabled` | false | Turns the module on. |
| `ToggleKey` | no key | Hotkey that switches the pass on and off. |
| `HookMethod` | 1 | Where the pass runs: 0 auto, 1 on the upscaler's output, 2 at present. |
| `TransferStrength` | 1.0 | How far the frame moves toward the model's picture; 0 gives the game's frame back exactly. |
| `WorkingScale` | 1.0 on RTX 40+, 0.5 on RTX 20/30 | The fraction of the frame the model works at. |
| `WhitePointSource` | 1 | Where the white point comes from: 0 paper white, 1 the game's exposure, 2 an exposure scan. |
| `ModelLoader` | forwarder | `forwarder`, or `direct` (OptiScaler calls the model itself). |
| `HighlightRecovery` | false | Keeps bright highlights closer to the game's frame (from RTX Remix). |
| `Cadence` | 1 | Run the model on one frame in N and carry its result in between (1 is every frame). |
| `StabilizerStrength` | 0 | Holds still pixels steady (0 is off). |
| `GpuTiming` | false | Fence-confirmed GPU timing for the pass. |
| `Compare` | 0 | 1 side by side, 2 wipe (0 is off). |

## Build from source

Windows-only C++ (MSVC, `OptiScaler.sln`, x64). On Linux, `./build-local.sh` builds with MSVC under
Wine (it runs [`tools/build-local.sh`](tools/README.md)). The Build section of [`CLAUDE.md`](CLAUDE.md#build)
has the details: the Wine stall recovery, formatting with clang-format 22, CI.

Do not run `git submodule update --init --recursive` blindly: `external/FidelityFX-SDK` is very large and
is deliberately left unchecked-out.

## Tests

```bash
python3 tests/run_all.py            # host tier: Python and g++/clang++, about a minute
python3 tests/run_all.py --list     # suites, tiers, and what can run here
python3 tests/run_all.py --tier all # adds the Wine suites
```

The host tier checks decision logic, configuration round-trips, bridge lifetime and more against fakes,
under sanitizers. It says nothing about image quality or performance. The Wine tier builds real DLLs and
drives real D3D12 and Vulkan. See [`tests/README.md`](tests/README.md).

## Layout and documentation

- [`retoque/`](retoque/dlssnr/README.md): the Neural Rendering module (`dlssnr/`) and its shaders
  (`shaders/dlssnr/`). Start with [`retoque/dlssnr/README.md`](retoque/dlssnr/README.md).
- [`docs/`](docs/README.md): design notes ([index by topic](docs/dlssnr/README.md)), Linux/Proton notes,
  guides and history. The rules every change follows are in
  [`docs/dlssnr/design/DEVELOPMENT.md`](docs/dlssnr/design/DEVELOPMENT.md).
- [`tools/`](tools/README.md): build, deploy, release and log-summary scripts.
- [`tests/`](tests/README.md): the test suites.
- `OptiScaler/`: OptiScaler's tree, kept at upstream's paths so upstream merges stay easy.
- [`Config.md`](Config.md): every INI key.

## Credits

Retoque stands on other people's work. Full licence texts are in [`Licenses/`](Licenses/).

- **[OptiScaler](https://github.com/optiscaler/OptiScaler)**, created by
  [cdozdil](https://github.com/cdozdil), and its contributors. GPL-3.0.
- **[Dagherbou's OptiScaler_DLSSNR](https://github.com/Dagherbou/OptiScaler_DLSSNR)**, the DLSS-NR fork
  this module started from.
- **[RenoDX](https://github.com/clshortfuse/renodx)** (clshortfuse): the colour composition. clshortfuse
  also made the patched 310.8.2.0 model build.
- **[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix)**: the highlight recovery step.
- **[optimizer-fps-dlss5](https://github.com/BeliyG3/optimizer-fps-dlss5)** (BeliyG3, MIT): the peripheral
  compression mapping and the model cadence carry.
- **[DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder)** (jlrouzies-fr, MIT): the output
  stabilizer's gate and the reduced-scale guide fixes.
- **[dlss5-bridge](https://github.com/NIGos/dlss5-bridge)** (NIGos, MIT): the NGX loader's feature-18
  route.
- **[OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)**
  (wilsjo2, GPL-3.0): the direct model loader, DLSS enlargement and the peripheral compression
  integration.
- **[OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)**
  (janblade, GPL-3.0): "Measure detail", per-thread scopes, and the model cadence and ViT-skip ideas.
- **[scottmudge's OptiScaler_DLSSNR](https://github.com/scottmudge/OptiScaler_DLSSNR)**: the present hook
  and descriptor reuse, rewritten here.
- **[Skynizz](https://github.com/Skynizz/optiscaler-dlss5)**, **hhkbble** and **sqmq13**: the exposure
  latch, the residual transfer and signed-value fixes, adapted.
- **[KleberMotta](https://github.com/KleberMotta/OptiScaler-DLSS5-MFG-RTX40)**,
  [ImDreamt](https://github.com/ImDreamt/MFGAdaUnlock-RenoDx),
  [mavismmg](https://github.com/mavismmg/MFGAdaUnlock-RenoDx) and
  [dashdogy](https://github.com/dashdogy/RTX40MFG-Unlock): the DLSS-G snippet rule.
- **[AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)** (MIT parts): the
  optical flow behind `SynthMotion`.
- **[dxvk-nvapi](https://github.com/jp7677/dxvk-nvapi)** (jp7677): the NVIDIA Optical Flow headers (MIT).
- **y4my4my4m**, **RizziU** and **jackra1n**: Linux/Proton fixes from their OptiScaler pull requests
  ([#1138](https://github.com/optiscaler/OptiScaler/pull/1138),
  [#1131](https://github.com/optiscaler/OptiScaler/pull/1131),
  [#1163](https://github.com/optiscaler/OptiScaler/pull/1163)).

## Trademarks

DLSS, NVIDIA and RTX are trademarks of NVIDIA Corporation. Retoque is an independent project, not
affiliated with, endorsed by or supported by NVIDIA. Other names belong to their owners.

## License

GPL-3.0, inherited from OptiScaler: see [`LICENSE`](LICENSE). Third-party parts keep their own licences,
listed in [`Licenses/`](Licenses/). `Licenses/RenoDX_ATTRIBUTION.txt` must ship with any build you
distribute.

## Em português

O Retoque aplica o modelo DLSS 5 Neural Rendering da NVIDIA na imagem de qualquer jogo, no Linux (Proton)
e no Windows, usando o OptiScaler como base. Ele roda depois do upscaler do jogo (D3D12), em Vulkan, em
jogos D3D11 por uma ponte D3D11-para-D3D12 e, na hora do present, em jogos sem upscaler (como
emuladores). O arquivo do modelo (`nvngx_dlssnr.dll`) **não** vem junto: você precisa fornecê-lo. É um
projeto experimental, sem vínculo com a NVIDIA, sob GPL-3.0.
