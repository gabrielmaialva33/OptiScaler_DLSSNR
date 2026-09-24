# DLSS 5 Neural Rendering (`OptiScaler/dlssnr`)

A self-contained module that drives NVIDIA's DLSS Neural Rendering model (`nvngx_dlssnr.dll`, NGX
feature 18) over the frames OptiScaler already handles. Nothing in it is officially supported by
NVIDIA; the model ships in driver packages and is not redistributed here.

## For maintainers: how to remove it

There is no compile switch. There used to be one, `OPTI_DLSSNR`, and it was removed on purpose: once
the composition became an ordinary shader class beside the others, code behind an `#if` was code
nobody compiled and therefore nobody tested, and thirty-two guard lines across nine files made every
future refactor riskier for whoever maintains this next. The call sites are now what they always
claimed to be — one line each — so deleting them is the removal.

**The procedure, in full:**

1. Delete `OptiScaler/dlssnr/` and `OptiScaler/shaders/dlssnr/`.
2. Drop `dlssnr_forwarder.vcxproj` from the solution.
3. Delete the call sites below, and the `[DlssNr]` block in `Config.h` / `Config.cpp`.

Nothing else refers to it. The list grew with the present-time host, the exposure scan and the
Vulkan route; it was six files once, and a grep for `DlssNr` outside the two module directories is
what keeps it honest (last taken 2026-09-23).

| File | What the calls do |
|---|---|
| `inputs/NVNGX_DLSS_Dx12.cpp` | the pass after an upscale on each evaluate route, the pre-upscale scope, an exposure-scan note |
| `inputs/NVNGX_DLSS_Vk.cpp` | the Vulkan pass after an upscale |
| `upscalers/IFeature_Dx11wDx12.cpp` | the pass and pre-upscale scope inside the D3D11-on-D3D12 bridge, the D3D11 probe |
| `upscalers/IFeature_VkwDx12.cpp` | the pass and pre-upscale scope inside the Vulkan-on-D3D12 bridge |
| `wrapped/wrapped_swapchain.cpp` | the present-time pass (`RunPresentPass`) |
| `hooks/FG_Hooks.cpp` | the present-time pass from OptiScaler's frame-generation present |
| `with_dx12/dx11_with_dx12_sc.{h,cpp}` | the D3D11 bridge's present host (`PresentHost`) |
| `hooks/D3D12_Hooks.cpp` | exposure-scan resource notes |
| `resource_tracking/ResTrack_dx12.cpp` | exposure-scan resource notes and submission tracking |
| `hooks/Vulkan_Hooks.cpp` | the device extensions the Vulkan route needs (`VkExt`) |
| `menu/menu_common.cpp` | the settings panel, the timing row, the toggle key, compare tags, the scan meter |
| `Config.h` / `Config.cpp` | the `[DlssNr]` declarations and their read/write runs |

`shaders/output_scaling/OS_Dx12.h` also takes a downscaler argument NR passes; it defaults, so it
needs no edit.

The config block is contiguous and marked `removable as one block` at both ends, so it lifts out
whole rather than needing to be picked apart.

One change outside the module is **a genuine upstream fix, separable on its own and worth taking
regardless of this feature**: `shaders/output_scaling/OS_Dx12.cpp` sized its dispatch from the global
current feature rather than from the resources passed in. Those coincide for the conventional Output
Scaling chain, so the bug stayed invisible until something else called it.

## Files

The pass itself lives under `shaders/dlssnr/`, dispatched like every other shader here. What stays in
`dlssnr/` is the parts that are not the shader: the menu, the capture, the forwarder, the proxy
experiment.

| File | Role |
|---|---|
| `DlssNr.h` | umbrella header; documents the call sites |
| `DlssNrFeature_Dx12.h` | the namespace-level API the menu and the call sites use |
| `DlssNr_Menu.cpp` | the settings panel |
| `DlssNr_Capture.h` | matched before/after frame dumps |
| `DlssNr_Proxy.h/.cpp` | the experiment in reaching the model through the driver core instead of the forwarder; see `FORWARDER_INVESTIGATION.md` |
| `forwarder/` | the caller-gate shim, built by `dlssnr_forwarder.vcxproj` into the release layout |
| `shaders/dlssnr/DlssNr_Dx12.h/.cpp` | the pass: forwarder loading, feature lifetime, the evaluate path, encode/resolve orchestration, capture |
| `shaders/dlssnr/DlssNr_Common.h` | the constant buffer, shared by the host and the shader |
| `shaders/dlssnr/precompile/dlssnr.hlsl` | **the live shader**: encode (scale and sRGB-encode with a soft knee), area downsample, resolve (RenoDX's two-branch composition, OkLab hue correction, AP1 clamp, the guard) |
| `shaders/dlssnr/precompile/DlssNr_Shader.h` | that shader compiled, as bytes |

### Editing the shader

`dlssnr.hlsl` is **precompiled**; editing it alone changes nothing. Rebuild **both** targets and
both headers (`design/DEVELOPMENT.md` invariant 6 is the authority):

```
cd OptiScaler/shaders/dlssnr/precompile
../../shader_tools/dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr.hlsl -Fo DlssNr_Shader.cso
../../shader_tools/dxc.exe -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi dlssnr.hlsl -Fo DlssNr_Shader_Vk.spv
python ../../shader_tools/create_header.py DlssNr_Shader.cso DlssNr_Shader.h DlssNr_cso
python ../../shader_tools/create_header.py DlssNr_Shader_Vk.spv DlssNr_Shader_Vk.h dlssnr_spv
```

On Linux run `dxc.exe` under the msvc-wine prefix (`WINEPREFIX=~/.local/opt/msvc-wineprefix wine
...`), and not while a build is using it. The committed `.cso` is DXIL from dxc `cs_6_0`; an older
revision of this section said fxc `cs_5_0`, which no longer reproduces it. `tests/nr-invariants`
checks that each header is exactly the bytecode beside it; that the bytecode is the compile of the
current `.hlsl` is yours to check.

## Attribution

The colour composition -- the two-branch luminance ratio, the OkLab hue correction and the blend
between a luminance-only result and the model's own colour -- is **taken from RenoDX's DLSS 5 addon
by clshortfuse** (https://github.com/clshortfuse/renodx). It is their design, reimplemented here with
different names; that does not make it ours. See `Licenses/RenoDX_ATTRIBUTION.txt`, which must carry
their upstream licence text before any build is distributed.

What is not theirs: the OkLab matrices are Bjorn Ottosson's published constants, and the AP1, sRGB
and PQ transforms are standard colour science.

## Why a forwarder DLL exists

The model's snippet resolves the module that owns its caller's return address and refuses any whose
path does not contain `nvngx.dll`. The forwarder (`nvngx.dll_dlssnr.dll`, ~13 KB) exists only to
satisfy that check; every NGX call to the model originates from it. It contains no NVIDIA code, is
part of the solution, and builds with everything else.

## Design notes worth knowing before changing anything

- **Ratio composition, not a delta.** The model is shown an encoded proxy; what it returns is
  composed back as a ratio against the original's luminance, scaled by a measured slope, with the
  chroma added. Composing it additively — which earlier revisions did — discards the model's
  behaviour in highlights and makes every arrangement look alike. At strength zero the frame is
  bit-identical, always.
- **Create-time and live parameters.** The preset (the weight set) is read when the feature is
  built, so a preset change rebuilds it after a settle, as do resolution and placement. Style,
  intensity, local structure and tone, skin and the auto mask are read at every evaluate and apply
  without a rebuild (`748f8896`); a change pulses a history reset. That was once measured the other
  way, and the README said so; the RenoDX trace in neural-amd
  (`handoffs/RESULTADO-nvidia-preset-style-ets2-20260921.md`) shows the model taking a Style change
  with no CreateFeature and logging its own "reset temporal history ... after control change", and
  with the model's diagnostics now in `OptiScaler.log` (`DlssNr_ModelLog`) a session shows it
  directly. The driver's parameter block is not the SDK header's vtable (floats sit at slot 6); the
  forwarder probes it. Rebuilding every frame exhausts the driver's latches and the feature stops
  responding until the process restarts, which is why the rebuild is debounced.
- **Never free under the GPU.** Every retired feature or surface is parked and freed 32 evaluates
  later; every internal feature is created on a private queue and fenced before use. Both rules were
  paid for with device hangs.
- **One lock.** Every caller is on the game's render thread now, but the D3D11-on-D3D12 bridge
  enters from its own call site, and the lock is CPU-side on a path that already records command
  lists. It was added after a period of crashes that looked random and were not.
- **Temporal filtering of the model's answer was measured to be a dead end** (twice, including with
  a trained DLAA pass): the model re-decides detail with the framing, so old answers do not belong
  to new frames. There is no accumulator; the composition is re-anchored to the model every frame
  instead, which is what makes it steady.
- **The model's own UI correction went with it.** It only ever acted on a UI layer the game tagged
  through Streamline, which almost no title does, and it could not be shown to change anything when
  one did. Removing it removed the Streamline tag hook as well, so the module no longer touches that
  file at all. The model is created with the parameter at its own default.
- **HUD detection was tried and removed.** Measured with grain, chromatic aberration and depth of
  field all off, a static HUD pixel still scored 0.31 on the "did not change" test, because game
  interfaces are translucent and animated. Separation from the world was 2.5:1 — not a detector at
  any threshold. The interface is safe because the pass runs before it is drawn, not because
  anything looks for it.
- **The split pipeline was removed.** It ran Ray Reconstruction at 1:1, the model on that frame,
  then an internal Super Resolution pass to the target size, to give the model a real temporal
  accumulator behind it. It was removed once the plain path did the same job — but note that the
  plain path had a bug that stopped it running the model at all, so the split was never fairly
  compared. If detail shimmers in motion, that is the thing to look at again; it is in the history.
