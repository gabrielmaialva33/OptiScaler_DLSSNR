# Titles that call no upscaler at all — how the rest of the scene handles them

Status: **survey, with sources checked one by one.** No design and no code here. It exists because the
question "can this run in a game without DLSS" has two different answers depending on what the asker
means, and because the surrounding ecosystem has already tried every route worth trying.

## The two questions, and why only one of them is open

**A game with FSR2, FSR3, FfxApi or XeSS but no DLSS already works today.** Those inputs are
intercepted, normalised into an `NVSDK_NGX_Parameter` block, and dispatched through
`NVSDK_NGX_D3D12_EvaluateFeature` — for instance `inputs/FSR3_Dx12.cpp:380` and `:576`. That is the
same evaluate the neural pass hooks, and the guard inside it is
`feature != NVSDK_NGX_Feature_FrameGeneration` (`inputs/NVNGX_DLSS_Dx12.cpp:1214`) — *is this an
upscale*, not *is this DLSS*. The game never needs to know what DLSS is.

**A game that calls no upscaler at all does not work, and cannot without new code.** Every site that
dispatches the pass is in `inputs/NVNGX_DLSS_Dx12.cpp`, `inputs/NVNGX_DLSS_Vk.cpp`, or the
`upscalers/IFeature_Dx11wDx12.cpp` and `IFeature_VkwDx12.cpp` bridges. All of them require somebody to
call an upscaler. Nobody does, so the module is inert — correctly, but it puts emulators, older
titles and anything that renders and presents directly out of reach.

## Three routes exist. Only one is general.

### 1. Replace the engine's TAA — best quality, not generalisable

This is the standard method for high-quality upscaling mods, and the reason it is good is structural.
A game with modern TAA already produces everything a temporal upscaler wants: a projection matrix with
subpixel jitter, screen-space motion vectors, and scene depth. The mod intercepts the engine's own TAA
pass, takes the HDR colour, depth and velocity render targets, cancels the native TAA shader, feeds
them to FSR2 or DLSS, and hands the reconstructed image back into the pipeline. **The engine then
draws its HUD on top, at native resolution, untouched** — which is why UI never degrades on this
route.

Confirmed implementations, licences checked:

| engine | project | licence |
|---|---|---|
| RE Engine (Capcom) | `praydog/REFramework`, branch `pd-upscaler`, intercepting `via.render.RenderPipeline` | MIT |
| Creation Engine | PureDark's *Skyrim Upscaler*, distributed on NexusMods | not open |
| FromSoftware | PureDark, hooking the camera projection matrix to inject jitter | not open |
| Unreal Engine 4/5 | `PostProcessTemporalAA` / `FTemporalAAHistory` are a stable target | various |

One correction from checking: `skyrim-community-shaders` **is** GPL-3.0, but it is a shader framework
(PBR, shadows, lighting) that interacts with the native TAA. It is not the project that injects
DLSS/FSR2 there. An earlier draft of this note said it was.

**This route is unavailable to us.** It needs per-engine reverse engineering — memory layouts and
offsets, game by game. Nothing reachable from a `dxgi.dll` swapchain wrapper.

### 2. Capture the window from outside the process

Lossless Scaling, and Magpie's experimental Feature 18 filter. Application-agnostic, works with any
emulator on Windows.

Not available on Linux: window capture there depends on `Windows.Graphics.Capture`, Desktop
Duplication and DWM composition, none of which Wine implements. It also captures the post-tonemapped
desktop image and adds an inter-process copy.

### 3. Hook Present inside the process — the only general route left

ReShade proves the presentation moment is a universal, robust hook point, and it is where the DLSS 5
ecosystem actually runs Feature 18 in titles with no upscaler. This is what
[nr-present-hook.md](nr-present-hook.md) designs.

## It has been done. Concretely, not in theory.

| project | where Feature 18 ran | licence |
|---|---|---|
| `jlrouzies-fr/DLSS5-Feeder` (via ReShade) | Worms Ultimate Mayhem (32-bit OpenGL through D3D12 interop), Tomb Raider 2013 (D3D11), D3D9 and D3D10 titles | — |
| `NIGos/dlss5-bridge` ("Substitute Contract", `synth=1`) | Baldur's Gate 3 in D3D11 with no DLSS; Arknights: Endfield with DLAA + MFG 4x + neural rendering together | MIT |
| `SAOG0721/Magpie` | window level, any game or emulator | GPL-3.0 |
| `SAOG0721/DaVinci-Resolve-DLSS5` | continuous video, zeroed depth and motion | MIT |

The Tomb Raider case is a user report on an issue rather than a maintainer claim; the rest are the
projects' own documentation.

## What breaks when the model sees a finished frame

Three of these have public sources and one does not. Kept separate on purpose.

- **Text and subtitles lose their hard edges.** The model reads a font's abrupt contrast as
  high-frequency noise. `NIGos/dlss5-bridge`'s README says it plainly — *"text softens and dense
  foliage smears"* — and Magpie's warns that *"image processing can affect text and UI"*.
- **Dynamic 2D HUD and minimaps ghost and warp** under fast camera motion, because a temporal model
  reprojects the UI with the 3D scene's motion vectors. Magpie documents *"temporal effects may also
  produce ghosting"*; `praydog/REFramework` issue #670 is titled *"UI blurry (pd-upscaler)"*.
- **Pre-rendered video pulses its grain and flickers at cuts.** `bmitch87/DLSS5VKLayer` answers this
  with a luma-based scene-cut detector, `DLSSNR_SCENE_CUT_THRESHOLD=55`, that forces `DLSSNR.Reset=1`.
- **Halos around translucent menus** — *no source found*. It is a plausible consequence of temporal
  accumulation over unmasked alpha surfaces, and it is recorded here as a hypothesis, not a finding.

## The parameter for this is not the one we assumed

An external survey twice reported, as established, that `DLSSNR.UseAutoMask = 1` is the model's own
HUD classifier — high-contrast edges without coherent motion given passthrough weights, "confirmed" in
three projects. **It is not.** Challenged and re-checked against primary sources:

- `UseAutoMask` is the **Auto Skin Mask**, coupled to `skinStructureStrength`: neural skin
  segmentation for facial detail. This tree presents it under exactly that name
  (`dlssnr/DlssNr_Menu.cpp:254,262`), declared beside `DlssNrSkinStructure` (`Config.h:277-278`).
- The UI parameter is a **separate boolean**, `UI Correction`, exposed independently in Magpie's
  `DLSSNRFilter.cpp:48-49` and in the DaVinci filter's parameter table.

**And this tree already passes it.** `DLSSNR.UICorrection` is plumbed through all three forwarder
create paths (`dlssnr/forwarder/dlssnr_forwarder.cpp:652`, `:748`, `:880`) and named in
`shaders/dlssnr/DlssNr_Common.h:266`. The D3D12 path hands it a hard-coded `1`, with this comment
(`shaders/dlssnr/DlssNr_Dx12.cpp:2556-2558`):

> UI correction at the model's own default: with no UI layer fed to it there is nothing for it to
> correct.

That is true of the route it was written for. The pass runs after the upscaler and before the HUD, so
there is no UI in the frame. **It stops being true the moment a present host exists**, because that
frame has HUD, text and menus in it — and `UICorrection` goes from inert to the parameter that
decides whether the route is usable. There is no config key and no menu control for it today; adding
them belongs to the present host, not before it.

## What this changes about the plan

Nothing about the direction, two things about the content.

The present host remains the only general route, and the case for it is stronger than "it would be
nice": it is what four independent projects converged on for exactly this problem. The TAA route is
better and permanently out of reach for a generic wrapper — worth knowing so nobody proposes it again.

And the HUD problem, which [nr-present-hook.md](nr-present-hook.md) lists as unanswered, has a named
parameter to try rather than only a compute-shader mask to write. Whether `UICorrection` is any good
is unmeasured; what changed is that there is something specific to measure.

## Sources checked

Every claim above was put back to its primary source before being written down. Six held, two did
not: `skyrim-community-shaders` as the Creation Engine TAA replacement, and the translucent-menu
halos. Both are corrected in place rather than removed, so the next reader can see what was believed
and why it failed.
