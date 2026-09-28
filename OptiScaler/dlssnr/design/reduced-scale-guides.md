# Guides and motion scale below the frame's size

Status: **built behind two default-on keys; host-tested; fix 1 A/B'd in Cyberpunk 2077 on 2026-09-28**
(see [Measured](#measured)). Written 2026-09-27. The guide resample's bytecode is in the tree since
`c95ad19c`; a build without it reports the resample unavailable and only the motion-scale fix is live.

## Where this comes from

Both faults and both fixes are **Jean-Laurent Rouzies's (jlrouzies-fr)**. He found them in wilsjo2's
[OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass)
(GPL-3.0) and fixed them in
[his fork's v0.8.92](https://github.com/jlrouzies-fr/OptiScaler-DLSSNR-PreSR-Multipass/releases/tag/v0.8.92),
which is v0.8.91 plus those changes, offered upstream as a pull request.

- **The symptom**, from the DLSS5-Feeder README: "Model resolution below 100% flickers on wilsjo2's
  builds up to v0.8.91 and keeps 'settling' for a few frames after the camera stops, at every scale
  below 100%."
- **Fix 1** (`MatchGuides`, commit `4a83ccff`). His A/B was in Fable Anniversary at 4K: 70% was as
  steady as 100% with the fix and flickered without it, on the same build.
- **Fix 2** (`RenderMotionScale`, commits `4e3b3d7c` and `75e2cd5c`). The first version converted
  the scale in every case and so broke 100%. The second measures it against the motion texture the
  model actually reads. That second rule is the one taken here.
  - His Onimusha: Way of the Sword numbers: 4K, DLSS Performance, game scale 1920. The old conversion
    gave the model 1344.
- **Vulkan.** v0.8.92 also ported both fixes to his Vulkan path (`57aa64e0`). Here only fix 2 is on
  Vulkan so far.

His code touches files this tree does not have (`DlssNr_Dx12_Run.cpp`, `DlssNr_Dx12_Models.cpp`), so
nothing was copied. The rules are re-derived and pinned by our own tests. The comments at each site
credit him.

## This tree had the same shape

In `DlssNr_Dx12::Dispatch`, below `WorkingScale=1`, the colour is box-resampled to the working size.
Then two things went wrong.

1. **Depth and motion stayed at the frame's size.** They went to the model as subrects of the game's
   own textures (`guideWidth`, `motionWidth`, the bases). Nothing documents that the model resamples a
   guide larger than its colour, and his A/B says it does not. Each pixel's depth and motion described
   some other place, so the history never lined up.
2. **The motion-vector scale was the game's scale times working / frame.** The model reads the scale in
   pixels of the motion texture it is handed. The game's scale is in pixels of the size its vectors
   are measured in, and for low-resolution vectors that is the render size. So the old conversion
   shrank every low-resolution vector below 100%, whatever the guides were.
   - At DLSS Performance with `WorkingScale=0.5`, render equals working, and every vector was halved.
   - At 100% the old conversion happened to be right: working / frame is 1, so the game's own scale
     went through.

**Who this hit.** Every NR run below 100% with guides larger than the model:
- every RTX 20 and 30 card on `WorkingScale=auto`, which resolves to 0.5 there
  ([working-scale-auto.md](working-scale-auto.md));
- Rafael's RTX 3060, which runs at 0.5 ([nr-present-hook.md](nr-present-hook.md));
- the before-upscale stage below 100%. Only fix 1 applies there, because the render-size colour *is*
  the vectors' reference size, so both formulas agree once the guides match.
- the present hosts' synthesized motion below 100%. Only fix 1 applies here too: the field is
  backbuffer-sized and in backbuffer pixels, and both formulas give 0.5 at 0.5.

**One case this does not settle: supersampling** (`WorkingScale > 1`) with real guides. The old
conversion doubled the scale at 2x. The new one passes the game's scale through, which is what the
model's reading implies, but nobody has compared the two in a game.

## The fix

`DlssNr_GuideMatch.h` holds the rules as pure functions. `tests/nr-before-upscale/guide_match_cases.cpp`
pins them with the numbers of the cases they were written for.

1. **Matched guides** (`[DlssNr] MatchGuides`, default true).
   - When either guide is larger than the model's input along some axis, depth and motion are
     point-resampled to the working size. They are handed over as a full zero-origin region.
   - The textures are `R32_FLOAT` for depth and `R32G32_FLOAT` for motion. Vectors in pixels reach the
     thousands, and at that size 16-bit float has lost the sub-pixel.
   - They are created at the working size. On a resolution change they are parked with the other
     per-size textures. Every pass of a chain, and the proxy path, reads the same pair.
   - **Where we differ from v0.8.92.** He resamples whenever the model is below 100%. We resample only
     when a guide is larger than the model.
     - A guide smaller than the model is the upscaler's ordinary contract, render resolution under
       display resolution. The model has always taken that as a subrect.
     - His own Onimusha case is exactly that shape: 1920 guides under a 2688 model at 70%. At 100%,
       with 1920 guides under 3840, it had always been steady.
     - Point-sampling up would only throw information away.
2. **Motion scale for the texture handed over** (`[DlssNr] RenderMotionScale`, default true).
   - The formula is `game scale x handed motion extent / reference extent`, per axis. The reference is
     the render size for low-resolution vectors, and the output size otherwise.
   - Unmatched, the handed texture is the game's region, so the scale is the game's own. Matched, the
     handed texture is at the working size.
   - `false` restores working / frame, for an A/B on one build.
   - The native Vulkan path has fix 2 only. Fix 1 needs a Vulkan resample shader, which it does not
     have yet, and the log says so once.

**A pass of its own, with optional bytecode.**
- The resample is `DlssNr_GuideMatch_Dx12` over `precompile/dlssnr_guides.hlsl`. It is not a new mode
  of the composition shader. `DlssNr_GuideMatch_Dx12.cpp` includes `precompile/DlssNr_Guides_Shader.h`
  only when `__has_include` finds it.
- Without that header, `Available()` is false and nothing is built. The model keeps the frame-size
  guides, with the corrected scale.
- The alternative was a new mode of `dlssnr.hlsl`, which nobody would have compiled. That is the silent
  failure mode: `nr-invariants` does not compare HLSL with bytecode, so a stale composition shader
  would run a mode it does not know.

**Build the shader** from `OptiScaler/shaders/dlssnr/precompile`, with dxc under the msvc-wine prefix.
Never do it while a DLL build is running.

```
WINEPREFIX=~/.local/opt/msvc-wineprefix wine ../../shader_tools/dxc.exe -T cs_6_0 -E CSMain -O3 \
    -Qstrip_debug -Qstrip_reflect -Fo DlssNr_Guides_Shader.cso dlssnr_guides.hlsl
python3 ../../shader_tools/create_header.py DlssNr_Guides_Shader.cso DlssNr_Guides_Shader.h DlssNr_Guides_cso
```

Then:
- add `('DlssNr_Guides_Shader.cso', 'DlssNr_Guides_Shader.h', 'DlssNr_Guides_cso', b'DXBC')` to
  `tests/nr-invariants/run.py`, next to the stabilizer's entry;
- list the header in `OptiScaler.vcxproj`;
- commit the `.cso` and the header together.

## What the log says

Once per change. These are the numbers the Cyberpunk case should print:

```
DLSS-NR guides matched to the working size: depth and motion 1720x720 for a 1720x720 model (the frame's guides are 2293x960 and 2293x960)
DLSS-NR model motion scale 1720.0 x 720.0: game scale 2293.0 x 960.0 measured against 2293x960 (render size, low-resolution vectors), motion texture 1720x720 (matched to the working size), model 1720x720
```

Without the bytecode, the first line reads `DLSS-NR guides: larger than the 1720x720 model and not
resampled (this build has no guide resample shader); the model gets the frame-size guides`.

## In-game test plan

Build the shader first, then the DLL, so the build carries `DlssNr_Guides_Shader.h`. Both keys are
read from the ini only, so restart the game between arms. Use the same save and the same spot.

1. **Cyberpunk 2077**, D3D12, 3440x1440, DLSS Quality (render 2293x960), Ray Reconstruction **off**.
   - Ini: `[DlssNr] Enabled=true`, `Stage=0`, `WorkingScale=0.5`, `GpuTiming=true`.
   - **Arm A**, the defaults: expect the two log lines above.
   - **Arm B**: `MatchGuides=false`, `RenderMotionScale=false`. The log reads "not resampled" and
     "legacy working/frame conversion", with scale 1146.5 x 480.0.
   - Pan, then stop the camera. Arm B should flicker and settle for a few frames after the stop. Arm A
     should be as steady as `WorkingScale=1`.
   - Record `model_ms` and `outside_model_ms` per arm. The resample is outside the model's window,
     so any cost it has shows in the second.
2. **Cyberpunk again, `Stage=1`.**
   - Expect `DLSS-NR before the upscaler: the model works on the 2293x960 render-size colour ...`.
   - Then expect guides matched to 1147x480: the model is render x 0.5.
   - The same A/B applies. Here only `MatchGuides` should change the picture: with matched guides both
     scale formulas agree.
3. **DOOM Eternal (Vulkan)**, DLSS Quality, `WorkingScale=0.5`. This checks fix 2 only.
   - Expect `DLSS-NR Vulkan model motion scale ...` with the game's scale passed through for the
     2293x960 region.
   - `Vulkan guides: larger than the 1720x720 model and not resampled` is the known gap.
4. **PCSX2 present route** (`~/Games/pcsx2-nr-test/run-state.sh`), `SynthMotion=true`,
   `WorkingScale=0.5`.
   - The backbuffer-sized field is resampled to half size, and the scale stays 0.5.
   - Compare the shimmer metric recorded in [synthesized-motion.md](synthesized-motion.md) against the
     same run at `MatchGuides=false`.
5. **Supersampling.** Any title with real guides at `WorkingScale=2`.
   - The scale now stays the game's. Compare `RenderMotionScale=true` against `false` on a pan.
   - This is the one case the new rule changes with no evidence from elsewhere.
6. **Then Rafael's 3060** at 0.5: Mortal Shell II and Subnautica 2, the after-upscale titles measured
   on it ([model-cost-across-architectures.md](model-cost-across-architectures.md)). That is where
   the flicker should go away.

## Measured

### Cyberpunk 2077, 2026-09-28: fix 1 confirmed, fix 2 not exercised

This was build `16dff6de` on the RTX 4090. The game ran at 3440x1440 with **Ray Reconstruction on**,
at native render size: the frame's guides were 3440x1440. So the run differed from plan item 1 in
two ways.

- **On the ray-reconstruction route, `RRWorkingScale` governs, not `WorkingScale`.** The ini's
  `WorkingScale=0.5` did nothing, and NR started at 3440x1440. Arm A reached 1720x720 from the
  "RR Working scale" slider, mid-session. Arm B set `RRWorkingScale=0.5` in the ini.
- **Render size equalled frame size**, so both scale formulas give 1720 x 720 and fix 2 changed
  nothing. The only difference between the arms was the size of the guides the model read.

| Arm | Log | Model (median) | Outside the model | Samples |
|---|---|---|---|---|
| Full size, reference | working size 3440x1440 | 6.77 ms | 0.18 ms | 132 |
| A, defaults | `guides matched to the working size: depth and motion 1720x720` | 2.89 ms | 0.24 ms | 196 |
| B, both keys `false` | `motion texture 3440x1440 (the game's region) ... legacy working/frame conversion` | 3.09 ms | 0.20 ms | 75 |

- **The picture:** the user reported "uma leve cintilação" (a slight scintillation) in arm B and
  nothing like it in arm A. That is the flicker jlrouzies-fr describes, gone with matched guides.
- **Cost:** the resample adds about 0.04 ms outside the model's window. Arm B's model window came out
  0.2 ms longer, over a quarter of arm A's samples, so treat that difference as unproven.
- **Frame rate** did not move in any arm: about 118 presents to 29 renders a second, with the game's
  4x frame generation and a frame cap. At 0.5 the gain is GPU headroom, not frames.

**Still open:** fix 2 needs a render size below the frame's (item 1 as planned, Ray Reconstruction
off, or `Stage=1`). Items 3 to 6 are not yet run either.
