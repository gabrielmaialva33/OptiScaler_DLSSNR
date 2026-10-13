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

And the HUD problem, which [nr-present-hook.md](nr-present-hook.md) lists as unanswered, had a named
parameter to try rather than only a compute-shader mask to write.

**It was measured on 2026-09-14 (`76feb2cb`), and it does nothing.** A presented frame carrying
hard-edged bitmap text, a translucent menu, thin minimap lines and a moving counter was put through
independently created feature-18 instances at `UICorrection` 0 and 1, over identical source and
history sequences, with repeats, runtime switches and verified parameter-block readbacks. **All 192
presented RGB8 outputs were identical.** That measurement stands. **The reading of it was wrong, and
is corrected here the next day.**

`UICorrection` is not a dead parameter. It is a **gate**. `kibblerz/DLSS5-Reshade-AIO`'s
`lab/PRIVATE-CONTRACT-FINDINGS.md:112-127` reports, from its own probing of the contract, that
`UICorrection=0` makes `DLSSNR.UI`, `DLSSNR.UIAlpha` and `DLSSNR.Backbuffer` output-inert, and that
with it enabled the alpha channel of `UI` becomes a protected-pixel mask: alpha zero leaves NR
active, alpha one bypasses it, a spatial alpha mask bypasses it only where marked, and the RGB of
`Backbuffer` supplies the pixels restored in the protected region. Their coherent test fed the
original input as `Backbuffer` with a rectangular `UIAlpha` and got the original pixels back inside
the rectangle with NR retained outside it.

That is exactly why our own measurement found nothing: this tree clears those three inputs, and
`76feb2cb` says so in its own commit message — *"Explicitly clear the absent separate UI/alpha/
backbuffer inputs, matching production."* **We measured a gate with nothing behind it.** Flipping it
could not have changed anything.

Two limits on this correction, both real. It is **a third party's testing, not ours**; nothing here
reproduces it, and the obvious reproduction is cheap — supply `Backbuffer` and a rectangular
`UIAlpha` in the loopback harness and see whether the rectangle comes back unprocessed. And even if
it reproduces, it supplies a **mechanism, not a mask**: a present host still does not know where the
HUD is. What changes is that protecting the HUD stops being "write a compute shader and hope the
model cooperates" and becomes "produce a mask and hand it to a documented contract".

## Sources checked

Every claim above was put back to its primary source before being written down. Six held, two did
not: `skyrim-community-shaders` as the Creation Engine TAA replacement, and the translucent-menu
halos. Both are corrected in place rather than removed, so the next reader can see what was believed
and why it failed.


## What the sibling projects actually run on an emulator (2026-09-14)

Ten of these projects were cloned and read (`tmp/`, gitignored). Two findings change what this note
concluded.

**The Feeder route runs with no guides at all on an emulator.** A complete DLSS5-Feeder install was
found on a PCSX2 — ReShade as `dxgi.dll`, `dlss5-feed.addon64` 0.12.0, `renodx-dlss5.addon64`, the NR
model — together with the log of a real 35-second session on an RTX 3060. Feature 18 ran
(`inline feature 18 evaluation succeeded (count=60, NR input 1920x1080)`). But its own probes report:

```
[feed] MV probe    (frame 600): mean |mv| 0,000 px, 0% non-zero  <-- DLSS is getting (almost) no motion vectors
[feed] Depth probe (frame 600): min 0, max 0, variance 0         <-- sampled depth is flat
```

with its motion estimator enabled (`DLSS5_MV_PROVIDER=3 (LumeniteFX Kernel) -> enabled`). The
architectural advantage this note credited the ReShade route with — real depth plus estimated motion
— **delivered nothing on the emulator**. In practice it ran on exactly the zeroed guides step 1 uses.
For emulators specifically, a present host with zero guides is not behind the state of the art; it is
the state of the art, minus a ReShade dependency.

**The motion-vector answer the Windows projects use is unavailable under Proton, and a better one is
not.** `pcdofafa/dlss5-for-all` (MIT) solves motion estimation with the **D3D12 Video Motion
Estimation** block — hardware, off the 3D queue, with measured cost (0.89 ms/frame for a 215x90 grid
at half resolution on an RTX 4080) and an honest failure mode written down: above its search range it
returns a *small* wrong vector, which no magnitude threshold distinguishes from slow real motion.

That door is shut here. vkd3d-proton implements no part of the D3D12 Video family: there is no
`d3d12video.idl` in its `include/`, and a repository search for `ID3D12VideoDevice` and
`VideoMotionEstimator` returns nothing while the control term `ID3D12CommandQueue` returns hits. (An
earlier `strings` check of Proton's `d3d12core.dll` appeared to show the same thing and was worthless
— its control term returned nothing either. It is recorded because the conclusion was right for the
wrong reason once.)

But **`VK_NV_optical_flow` is present on this workstation's driver, with a dedicated
`QUEUE_OPTICAL_FLOW_BIT_NV` queue family.** That is the Ada Optical Flow Accelerator reachable
natively, on its own queue, contending with neither the 3D queue nor the video encoder. If motion
vectors are ever fed to this pass on Linux, that is the door — and it is a route this fork would own
rather than port. Untested; what is established is only that the extension and the queue exist.

**Two interop hazards named by a sibling that read this tree.** `jlrouzies-fr/DLSS5-Feeder`'s
`src/feed_opti.h` is 242 lines about this fork, with measurements (OptiScaler-DLSSNR v0.2.0 as
`winmm.dll` on an RTX 5090: 300/300 evaluates, 3.4 ms with the neural pass on the dlss backend). It
records two things worth knowing: with our LoadLibrary redirect live, anything asking for `nvngx.dll`
receives our module, so Feeder plus this fork in one process fires **two neural passes**; and
`[DlssNr] ScanExposure` hooks resource creation looking for an exposure buffer a bare NGX client
never offers.

Licences, since these were read as references: `dlss5-for-all`, `DLSS5-Feeder`, `dlss5-bridge`,
`DLSS5-Swapper`, the Resolve filter and `DLSS5-Autopilot` are MIT; `DLSS5-Reshade-AIO` is Apache-2.0;
Magpie is GPL-3.0. **`DLSS5VKLayer` is AGPL-3.0 and must not be copied from.** `DLSS5-Universal`
carries no licence at all and was not cloned.
