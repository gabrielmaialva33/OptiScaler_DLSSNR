# Transfer 2: the model's edit enlarged by a private DLSS Super Resolution

Status: **implemented, opt-in, not yet measured in a game or on native Windows.** `Transfer=2`, shown
as "Matched residual + DLSS". The default stays 1. D3D12 and the two D3D12 bridges only.

## Where this came from

The mode is **wilsjo2's**, from
[OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
(GPL-3.0, the same licence text as this tree), v0.8.91 (`f45ccf3a`). The same code is upstream's open
PR optiscaler/OptiScaler#1158, commit `62c528b4`, as "Matched Residual + DLSS".

- The carrier, its 1/64 scale, the decode and its 0.999 clamp, the unit exposure, and the private
  SR's creation and evaluation parameters are ported from his `dlssnr.hlsl`, `DlssNr_Dx12_Enlarge.cpp`
  and `DlssNr_Upscaler_Dx12.cpp`.
- The residual and `CubeScaleResidual` it lands through are **hhkbble's**, already credited in the
  shader.
- Each ported site says so in a comment and says what was changed, as GPL-3 section 5(a) asks.

Value 2 means the same thing in both forks; that is why [sharp-residual.md](sharp-residual.md) took 3
for the Catmull-Rom mode. His 3 and 4 ("Lighting + colour", with and without DLSS) are a different
mode that this tree does not have. An ini carried over from his builds with 3 runs the sharp mode
here, and with 4 runs Classic.

## What it does

Transfer 1 enlarges the model's edit with the sampler's bilinear filter. Bilinear cannot put back
what the small raster could not hold, and [sharp-residual.md](sharp-residual.md) records that the
band near the small raster's limit is where the model's fine structure lives. Transfer 3 answers that
with a sharper kernel. Transfer 2 answers it with a temporal upscaler: DLSS sees the edit move with
the scene over many frames, so it can place sub-pixel detail that no single small frame contains.

1. **The carrier.** A new mode of the composition shader (5) takes the edit at the model's working
   size, `d = answer - proxy` in the same linear space the resolve works in, and writes
   `0.5 + 0.5 * d / (1/64 + |d|)` to an RGBA16F texture.
   - 0.5 is "no change". The signed compression keeps darkening representable in an image DLSS treats
     as LDR colour.
   - The 1/64 scale is there for FP16, whose step just above 0.5 is 1/2048. At unit scale an edit of
     0.0003 sits a third of a step above 0.5 and comes back as no edit at all; at 1/64 it lands nineteen
     steps above and comes back within 4%. wilsjo2 measured the unit scale as speckles in KCD2.
     `tests/nr-enlarge` holds both numbers.
   - **Deviation:** where the model's answer is empty (all channels at or below 1e-5, the resolve's own
     `emptyModel` test), the carrier is 0.5 instead of a large darkening. Transfer 1 hands such a pixel
     back untouched; a carrier of "black minus proxy" would have DLSS spread that darkening around it.
2. **The enlargement.** A private DLSS Super Resolution feature takes the carrier from the working size
   to the frame's size. It is never Ray Reconstruction, and never the game's feature:
   - its own parameter table (`AllocateParameters`) and its own handle, so its history is its own;
   - called through `NVNGXProxy`, the raw driver core, not through OptiScaler's exported
     `NVSDK_NGX_D3D12_*` entry points, so no `IFeature` is made and nothing re-enters the NR hook;
   - created LDR (no `IsHDR`), `MVLowRes`, `DepthInverted` when the game's depth is, quality
     `MaxQuality` whatever the real ratio is, node masks 1, no preset hint;
   - evaluated with zero jitter (the frame it enlarges is already de-jittered), a 1x1 R32 exposure
     texture holding 1 (shader mode 6), pre-exposure 1, exposure scale 1, sharpness 0, the render
     subrect at the working size, and the CPU time between its evaluations as the frame time.
3. **The guides.** Depth and motion at the working size. When the model's own guides were matched
   ([reduced-scale-guides.md](reduced-scale-guides.md)) the same pair is reused. When the game's
   regions already are the working size at the origin they are handed over as they are. Otherwise the
   existing `GuideMatch` resample fills a private pair. **Deviation:** wilsjo2's mode 10 did this with
   region fields smuggled through unrelated cbuffer members; that is not ported.
   - Motion scale is `ModelMotionScale(game scale, working size, motion reference)`, the rule the model
     itself is given, because every motion texture handed over is at the working size and keeps the
     game's units. While frame hold is on it is 0.
4. **The resolve.** The resolve is sent Transfer 2 and the enlarged carrier in the model's slot. It
   decodes `edit = (1/64) * c / (1 - |c|)` with `c = clamp(2 * carrier - 1, +-0.999)`. The clamp is
   there because DLSS can ring outside the carrier's range and the inverse has poles at +-1; at the
   clamp an edit is 15.6, which the cube scaling below bounds anyway. Then it is Transfer 1:
   `fullProxy` rebuilt with the encode's curve (and its passthrough gate), and
   `model = CubeScaleResidual(fullProxy, fullProxy + edit)`. The replace modes decode that `model`, not
   the carrier. The Difference debug view shows the decoded edit, the one actually applied.

## When it runs, and what runs instead

`DlssNr::Enlarge::Decide` (`dlssnr/DlssNr_Enlarge.h`) is the whole rule. Transfer 2 is sent only when
DLSS ran this frame; everything else is sent **Transfer 1**, matched residual, which is the same edit
enlarged with bilinear. The menu shows a status line with the reason while 2 is selected.

| Condition | Sent | Private SR |
|---|---|---|
| Transfer is not 2 | the configured value, unchanged | none; released if one exists |
| Native Vulkan | 1 (mapped at the Vulkan resolve) | none |
| Model at or above the frame's size | 1 | released |
| Before the upscaler (Stage 1) | 1 | kept, idle |
| No real guides (zero-guide hosts: `AllowSupersampling` false) | 1 | kept, idle |
| Guides not at the working size and no resample shader in the build | 1 | kept, idle |
| Debug view "model" | 1 | kept, idle |
| Failed this session | 1, until Retry | released |
| Not built yet, or built and its creation not yet past a present | 1 | created / waiting |
| Otherwise | **2** | evaluated |

**Deviations from wilsjo2, on purpose:**

- **Matched residual, not the clean frame, while it warms up or after a failure.** His build shows
  the frame without NR on those frames. That turns one creation into a visible flash of a different
  picture, and a failure into "NR silently off". Transfer 1 is the nearest picture there is.
- **Only below the frame's size, decided from the sizes.** His condition was `reduced`, which is true
  when the model supersamples too, while his status code released the feature whenever the working
  scale was 1 or more. So Transfer 2 above 100% created and released DLSS every frame. Here the model
  must be at or below the frame on both axes and below it on one; at 100% and above nothing is
  created. (Above 100% the supersampling down-leg already brings the answer to frame size.)
- **Before-upscale and zero guides are refused.** DLSS needs real motion to accumulate anything, and
  before the upscaler the frame is jittered render-size colour with no edit to enlarge to the frame.
- **History reset on hold edges** as well as his set: the model's reset, the game's reset, the first
  evaluation of a feature, and any NR frame on which the SR did not run (the frame counter is not
  the one before).

## Lifetime

- **Never created and evaluated in one command list.** The creation frame is sent Transfer 1 and the
  first evaluation waits for a later present (`Enlarge::CreationCrossed`; on a route whose presents
  are not counted, two NR frames, one more than the model waits), and, where submissions are tracked,
  for the creation's recording to have completed. A recording the tracker refuses creates nothing.
- **Never freed under the GPU.** The feature, its parameter table and its five textures (carrier,
  enlarged output, exposure, private depth and motion) are one bundle, parked as one entry in the
  retirement list and released when its last recording is done, NGX first. The NGX release happens
  after the bundle has left the list: releasing a feature can re-enter the queue hooks, and the list
  must not be mid-mutation then (his `Enlarge.cpp:17`).
- **Parked** on a change of working size, frame size, depth direction or device; when Transfer leaves
  2 or the model reaches the frame's size; on failure; on Retry.
- **The cap.** The parked-object limit stays 32. A rebuild parks at most 16 objects plus the six of
  the other colour format's set; the bundle is one more.
- **Shutdown** releases the bundle like everything else, except while the process is exiting, when no
  NGX call is made at all: the handle and table are dropped, as OptiScaler's own DLSS feature does.
- **The game shutting NGX down** (`NVSDK_NGX_D3D12_Shutdown` with OptiScaler's DLSS backend) makes the
  proxy's getters return null. The next evaluate then fails, the mode falls back to Transfer 1 and
  says so, and the release is skipped rather than called into a torn-down core.

## Descriptors, timing, the stabilizer

- **Descriptor ring.** With Transfer 2 a frame records up to six composition dispatches: the meter
  (game exposure only), encode, downsample, carrier, unit exposure, resolve. `DLSSNR_NUM_OF_HEAPS`
  goes from 48 to 64, which keeps more than ten frames between a slot's reuses. The guide-match ring
  stays at 16: the private resample runs only when the model's own did not, so one per frame still.
- **GPU timing.** The private SR runs between the model's end marker and the pass's end, so it lands
  in `outside_model_ms` and in the total. The timing contract carries `effective_transfer=` and
  `enlarge=` (the reason token), so warm-up samples and DLSS samples are different contracts. A frame
  planned as 2 on which DLSS then failed is rejected.
- **The composition line and the stabilizer** read the transfer the resolve is sent, so a change from
  1 to 2 at the end of warm-up is logged and drops the stabilizer's history.

## Invariants

- **Default-identical.** Transfer 0, 1 and 3 take exactly the code they took before, on the host and
  in the shader. No NGX call, allocation or dispatch happens unless Transfer is 2. The only change they
  see is two more fields in the timing contract's text.
- **No cbuffer growth.** 1/64 and 0.999 are shader constants; `DlssNrConstants` is untouched. Two new
  mode numbers in `DlssNr_Common.h`.
- **Passthrough.** The carrier is taken in the space the resolve decodes into, with the same
  passthrough gate, and the resolve rebuilds `fullProxy` through the existing gated expression.
- **Both shader targets are rebuilt.** The Vulkan bytecode carries modes 5 and 6 and the decode but
  never runs them: the Vulkan resolve maps 2 to 1 (`Enlarge::VulkanTransfer`), which is required, not
  cosmetic. Without it the Vulkan resolve would decode its plain small answer as a carrier.
- **No new key.** `[DlssNr] Transfer` gains a value; the four-point round trip already holds.

## Risks

- **D3D12 validity on native Windows.** Proton accepted a CBV that native D3D12 answered by removing
  the device ([reduced-scale-guides.md](reduced-scale-guides.md)). Resource states at the NGX call, the
  create/evaluate split and the ring under frame generation need a run on Rafael's PC.
- **Motion scale 0 while held.** The SDK helper treats a 0 scale as "unset, use 1"; the core may read 0
  as 0 or refuse it. A refusal shows as the failure status.
- **Scale.** wilsjo2 ran 43-56%. Below about 0.33 (this tree's floor is 0.25) is unknown.
- **Cost and memory are not measured.** The 4K output alone is about 66 MB, plus DLSS's own history.
  The NVIDIA app's DLSS preset override also applies to this private feature.
- wilsjo2 reports a faint grid in KCD2 and possibly Hogwarts, and ghosting is possible on fast motion.
- A title that ships only Ray Reconstruction may have no `nvngx_dlss.dll`; creation then fails and
  says so.
- NGX core initialisation is one flag for the process (`NVNGXProxy::_dx12Inited`), not one per device.
  On the bridges the core was initialised with the bridge's D3D12 device, which is the one the pass
  records on, so this holds today.

## How to evaluate

At Model resolution 50% on an after-upscale D3D12 title, compare Enlargement "Matched residual",
"Matched residual, sharp" and "Matched residual + DLSS" with the compare wipe, on a still scene with
fine texture and then on a slow pan. Expected: finer detail than 1 and 3, no new black pixels, no
halos, and nothing that settles for several frames after the camera stops. The status line must read
"Running" once warm. `GpuTiming=true` gives the cost as the change in `outside_model_ms`.

The loopback harness (`tests/dlssnr-loopback/run.py --transfer-ab`) can already create DLSS SR; a
Transfer 2 arm there is the next step for a measured A/B, with 0 and 1 byte-identical to the previous
bytecode as the guard.
