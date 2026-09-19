# Synthesized motion vectors for the no-upscaler NR path

Status: **design + unbuilt skeleton.** Not scheduled, not built, not wired. The skeleton files named
in [§10](#10-skeleton-files-in-this-change-unbuilt) exist but are in no `.vcxproj`, generate no
precompiled header, and are not referenced by any existing source. Written against the tree at
`c6273373` (2026-09-19), after the two measured runs recorded at the end of
[nr-dx11-bridge-host.md](nr-dx11-bridge-host.md) and the size-refusal behaviour committed in
`c6273373`. Reference line numbers point at the mined repos under `tmp/`, checked out for this work.

Read first: [nr-without-game-dlss.md](nr-without-game-dlss.md) (why guides are a quality axis, not a
gate, and the reference precedents), the "Zero guidance, colour and history" section of
[nr-dx11-bridge-host.md](nr-dx11-bridge-host.md), and [DlssNr_ZeroGuides.h](../DlssNr_ZeroGuides.h)
(the resource this replaces, and the contract it already meets). This note is the third step those two
defer: real motion, reconstructed from consecutive frames, with no game engine buffers.

## 1. The problem, stated as narrowly as it actually is

The present-time host feeds the model `DlssNr::ZeroGuides` — one `R32_FLOAT` depth and one
`R16G16_FLOAT` motion field, cleared to zero once and never rewritten
([DlssNr_ZeroGuides.h:9-26](../DlssNr_ZeroGuides.h)). Two facts, both measured this session, bound the
work:

- **At WorkingScale 1.0 the zero field is fine.** Divinity: Original Sin 2 and The Witcher 3 (DX11,
  no upscaler) ran tens of thousands of frames on zero guides with the model's history accumulating
  and **no smearing reported** ([nr-dx11-bridge-host.md](nr-dx11-bridge-host.md), "Measured
  (2026-09-18)"). So zeros are not disqualifying at native.
- **Above 1.0 the model refuses zero guides** with `NVSDK_NGX_Result_FAIL_InvalidParameter`, which
  `c6273373` now treats as a size refusal rather than a fatal error. The contrast is Crimson Desert,
  which supersamples 2x cleanly **because it hands the model the game's real motion and depth**. The
  model applies stricter guide validation when it has to synthesise pixels above native.

The present host already tests one hypothesis for why: that the refusal is *parameter* validation of a
degenerate contract (`MvScaleX=MvScaleY=1`, `DepthInverted=false` over a 1×1-looking field), not
inspection of the texture data. `DlssNr_PresentHost.cpp` sets `frame.MvScaleX = _width`,
`frame.MvScaleY = -_height`, `frame.DepthInverted = true` on the *zero* field for exactly this reason —
"zero motion times any scale is still zero, so this changes how the model reads the units, not the
data." If that experiment passes, supersampling may already work on zeros and this note is not needed
at 1.0. **This note covers the case it does not settle:** the model inspecting the motion texture and
requiring non-zero displacement to synthesise above native, and — separately — fast-camera content at
any scale, where a permanently still field is wrong on its own terms (a fast first-person pan keeps
almost nothing between consecutive frames; that is the case zeros have the most to get wrong, and the
one no D3D11-without-upscaler title on this workstation reaches, so it has never been measured).

The axis this note moves is **motion only**. Depth stays zero. Whether zero depth plus real motion
clears the above-1.0 refusal is the first open question ([§9](#9-open-questions-honest)); depth
reconstruction (a selection heuristic over the existing `OMSetRenderTargets` observations, or a
learned depth proxy) is a separate note and a separate cost, deferred exactly as
[nr-without-game-dlss.md](nr-without-game-dlss.md) step 3 leaves it.

## 2. The contract this has to meet — it is a drop-in for one accessor

The whole point of the existing `ZeroGuides` shape is that the pass reads `motion` as an opaque
`ID3D12Resource*` and asks nothing about where it came from. The pass
(`DlssNr_Dx12.cpp:2031` `EvaluateAtPresent` → the shared evaluate at `:3145`-`:3236`) reads the motion
texture through an SRV, applies `frame.MvScaleX/Y` (`:3189-3190`, `guideMvScaleX * mvToWorkX`), and
honours `frame.MotionSubrect*` / `frame.MotionVectorsLowResolution` for the valid region. So a
synthesized field is a drop-in **iff** it presents the same four things the zero field does:

| Property | Value | Why |
|---|---|---|
| Format | `DXGI_FORMAT_R16G16_FLOAT` | What the pass, the model, and every precedent expect (Magpie [extracted_pipeline_notes.md:149](../../../tmp/DLSS5VKLayer/extracted_pipeline_notes.md); bridge `synth.inc:700-712` calls it "what both shipped motion estimation shaders declare") |
| Extent | working resolution, same as `ZeroGuides.Motion()` (full source extent × WorkingScale) | Keeps `MotionVectorsLowResolution=false`, `mvToWork=1`, subrect = full extent — no new subrect maths in the pass |
| Units | current-to-previous displacement, in **working-resolution pixels** | Same convention Magpie binds (`MVecScale=1`, "MVs are in source pixels, current→previous", [extracted_pipeline_notes.md:130](../../../tmp/DLSS5VKLayer/extracted_pipeline_notes.md)) and NeuralScreen produces ("NGX consumes current-to-previous motion in pixel units", `guides.py:314`) |
| Rest state | wherever `GuideRestState(true)` says (today `DepthResourceBarrier`/`MVResourceBarrier` out of config) | The pass transitions the guide *from* the state the caller left it in; the producer must leave it there, exactly as `ZeroGuides::RecordClear`'s `restState` does |

Because the field is full-resolution and in working-res pixels, the integration sets **`MvScaleX = 1.0`,
`MvScaleY = ±1.0`** — replacing the present host's zero-field `MvScaleX = _width` / `MvScaleY = -_height`
units hack, which exists only to make a degenerate zero contract look valid. The `MvScaleY` sign is an
open question ([§9](#9-open-questions-honest)); estimation happens in image space (+x right, +y down)
and the model's expected sign is not documented. The bridge's evidence is that DLSS wants
`uv_prev - uv_cur` in +y-down UV (`synth.inc:69-74`: "CalcMV samples the previous frame at
`i.uv + total_motion` … the stored vector `uv_prev - uv_cur`"), i.e. current-to-previous — but it also
records a downstream `(-0.5, +0.5)` clip-to-UV scale, so the axis signs must be pinned by the
before/after capture, not asserted here.

## 3. Algorithm: coarse-to-fine block matching on a luma pyramid

**Chosen: pyramidal (coarse-to-fine) block matching over a small luma pyramid, with parabolic
sub-pixel refinement, computed at ~320 px wide and bilinearly upscaled to working resolution.** Not
optical hardware (no NVOFA), not CUDA, not OpenCV — a compute shader on the D3D12 queue the host
already owns.

Why this and not the alternatives:

- **Why block matching over dense gradient (Lucas–Kanade / DIS).** DIS is the measured precedent
  (`perseval-BLR/DLSS5-NeuralScreen`, `guides.py:93` `cv2.DISOpticalFlow_create`), and it is a
  *patch-based inverse-search, coarse-to-fine* method — block matching is the same skeleton with a
  simpler per-patch kernel (SAD/NCC instead of an inverse-compositional gradient step). Block matching
  has no linear solve, no iteration-convergence question, and a fixed, data-independent cost, which is
  what makes it the honest first thing to get right on the GPU. A single-level gradient method cannot
  represent the displacements that matter: a fast pan at 3440 wide moves 30+ px/frame, and DIS's own
  tails at a 32 px shift (`guides.py:34`, ultrafast max 31.6 px vs fast 8.5 px) are what a temporal
  network shows as smearing. The pyramid is not optional; the per-patch kernel is negotiable.
- **Why not DIS ported.** OpenCV is not in this DLL and will not be. Re-implementing DIS's
  inverse-compositional step as HLSL is a larger, subtler job than a coarse-to-fine SAD search, and the
  quality gap is not yet known to matter for a *guide* (the model consumes motion as a hint, not as the
  output). If block matching proves too coarse, the estimator kernel is swappable behind the same
  output field ([§10](#10-skeleton-files-in-this-change-unbuilt) keeps the kernel and the field
  separate for this reason).
- **Why ~320 px wide.** Every precedent estimates small and upscales. NeuralScreen runs flow at
  `flow_width=320` (`guides.py:66`, `scale = min(1.0, flow_width/width)`, even-rounded, min 64) and
  scales the vectors *before* the upscale because resize is linear and it is 115k elements instead of
  3M (`guides.py:334-340`, "verified: the two orders differ by 0.002, i.e. float16 rounding"). At
  3440×1440 that is a 320×134 estimation grid — ~43k cells — which makes the search itself trivially
  cheap and puts the only real cost in the full-res upscale.

### The three passes

1. **Luma + pyramid build.** Read the working colour (`_toWorking->Buffer()`, `R16G16B16A16_FLOAT`,
   the host's linear-ish proxy — [§5](#5-where-it-plugs-into-the-present-host)), compute luma, and
   area-downsample to the 320-wide base level, then build 2–3 mip levels. NeuralScreen's block average
   is "12×12 per cell at 4K → 320×180" (`TECHNICAL.md:280`) — an honest box average, not a bilinear
   point sample, so aliasing does not become false motion.
2. **Coarse-to-fine block match.** Per pyramid level, for each cell, search a small window
   (±4 px is plenty per level with 2–3 levels) against the *previous* frame's luma at the same level,
   scoring SAD over a patch (7×7 is the window NeuralScreen's trust test settled on, `guides.py:143`),
   seeded by the upscaled vector from the coarser level. Parabolic fit on the SAD minimum for
   sub-pixel. Emit `cur_to_prev` at the finest estimation level.
3. **Validate, scale, upscale.** Zero any vector below the noise floor (NeuralScreen: 0.5
   *working-res* px, `guides.py:102`, applied after scaling so a real 1–6 px scroll is not erased on
   the small grid). Multiply by `(workWidth/flowWidth, workHeight/flowHeight)` to reach working-res
   pixels, then bilinear-upscale the 2-channel field to the working-resolution `R16G16_FLOAT` output.

The optional but cheap **static-hypothesis guard** (NeuralScreen `guides.py:186-220`, `_moved`) is
worth porting after the field works at all: warp the previous frame by each vector and keep the vector
only if it explains the pixel better than standing still, by a margin. Measured there at +0.67 ms on
CPU for a 320-grid and it cut false vectors on static text from 0.6% to 0.01%, where a forward/backward
consistency check cost 3× as much and caught almost nothing (`guides.py:116-128`). On the GPU at this
grid it is close to free. It is a second slice, not the first.

## 4. Exact output contract

```
resource   one ID3D12Resource, DXGI_FORMAT_R16G16_FLOAT
extent     working width × working height (== ZeroGuides.Motion() extent)
allocation D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, DEFAULT heap
           (written by the upscale UAV; read by the pass as an SRV — same as ZeroGuides)
contents   .r = x displacement current→previous, .g = y displacement current→previous,
           both in working-resolution pixels; 0 where below the noise floor or history was reset
rest state left in GuideRestState(true), so the pass transitions it from where it actually is
frame      MvScaleX = 1.0f, MvScaleY = ±1.0f (sign per §9), DepthInverted unchanged (depth stays zero)
           Reset = true on any reset condition in §7, else the host's normal Reset
```

Contrast with the zero field it replaces: same format, same extent, same rest-state discipline; the
only differences are that the contents are non-zero and that `MvScale` returns to unit (the zero
field's `_width`/`-_height` units hack goes away when the data is real).

## 5. Where it plugs into the present host

`DlssNr_PresentHost::Record` (in `DlssNr_PresentHost.cpp`, owned by the integration worker — **not
touched here**) already:

1. converts the backbuffer into `_toWorking->Buffer()` (`R16G16B16A16_FLOAT`) and transitions it to
   `UNORDERED_ACCESS`, then
2. picks `motion = _guides.Motion()` (the zero field), unless `CapturedPresentGuides` handed over the
   game's real depth/motion this cycle (the Dx11wDx12 upscaler path), and
3. calls `EvaluateAtPresent(cmdList, _toWorking->Buffer(), depth, motion, frame, queue, &reason)`.

The synthesized producer slots in at exactly step 2, **only on the branch where there is no captured
real guide** (`CapturedPresentGuides` returned false — every frame of a title that ships no upscaler).
The integration is:

```
// after _toWorking->Dispatch, before EvaluateAtPresent, on the same cmdList and queue:
ID3D12Resource* motion = _guides.Motion();      // zero field, the fallback
frame.MvScaleX = (float) _width;                // the zero-field units hack, kept as fallback
frame.MvScaleY = -(float) _height;
if (_synthMotion.Record(device, cmdList, _toWorking->Buffer(), _width, _height,
                        <reset for this frame>) && _synthMotion.Ready())
{
    motion = _synthMotion.Motion();             // real field, working-res px, current→previous
    frame.MvScaleX = 1.0f;
    frame.MvScaleY = <sign>;                      // §9
    frame.Reset = frame.Reset || _synthMotion.ResetThisFrame();
}
// depth stays _guides.Depth() (zero) either way
```

The producer reads the **working colour**, not the raw backbuffer, for two reasons: it is already on
D3D12 in a known state, and luma from the model's own proxy is the same signal the model sees. It
records onto the caller's list and executes on the caller's one queue — no private submission, no CPU
wait, no second device. Its clear/confirm/abandon bookkeeping mirrors `ZeroGuides` and `PresentHost`
exactly (`ConfirmExecuted` advances the previous-frame swap only when the list ran; `AbandonRecording`
throws the swap away so a dropped list does not leave the estimator one frame stale — [§7](#7-reset-conditions)).

One host per admitted swapchain, one estimator per host, nothing shared between instances and nothing
reaching for a global — the same ownership rule `PresentHost` and `ZeroGuides` already hold.

## 6. Previous-frame storage — ping-pong luma, per host

The estimator needs last frame's luma at estimation resolution (and its pyramid). Storage is **two
small textures, ping-ponged**: the base is 320-wide × proportional × `R8_UNORM` (or `R16_FLOAT` if the
sub-pixel search wants more than 8 bits of luma — an open question), plus its 2–3 mip levels. At
320×134 with mips that is on the order of 60–90 KB per buffer — negligible next to the model weights
and the host's own RGBA16F working/output pair (the bridge note already tallies >500 MB before
weights). Each frame:

1. build current luma pyramid into buffer A,
2. block-match A against the previous pyramid in buffer B,
3. on `ConfirmExecuted`, swap A↔B so this frame's luma is next frame's previous.

The swap happens on confirm, not on record, so a recording that never executed does not poison the
history — the same discipline as `ZeroGuides::ConfirmExecuted`/`AbandonRecording` and `PresentHost`'s
`_serial`/`_resetOwed`.

## 7. Reset conditions

Emit a **zero field and `Reset=true`** whenever there is no trustworthy previous frame to difference
against:

| Condition | How it is known here | Precedent |
|---|---|---|
| First frame of the estimator | previous buffer never filled | `guides.py:301` (`previous_gray is None` → zero, reset) |
| Resize / extent or format change | producer re-allocates; previous is a different grid | `PresentHost::_Ensure` sets `_resetOwed=true` on size/format change |
| Scene cut | mean abs luma diff over the estimation grid exceeds a threshold | `guides.py:306-307` (`scene_score = mean(absdiff)/255`, `reset = scene_score > 0.24`) |
| Static screen | scene_score below a floor → skip the search entirely, emit zero (not a *reset*, just no work) | `guides.py:308-312` (`< 0.001`, measured 0.03 ms vs 3.9 ms for the DIS path) |
| Recording abandoned last frame | `AbandonRecording` cleared the swap | mirrors `PresentHost::AbandonRecording` |

Scene-cut detection is cheap here because the mean-abs-diff is a reduction over the already-built
320-wide luma — one pass, no extra estimation. The threshold `0.24` is NeuralScreen's, on video; it is
a starting value to be re-tuned on game content, and getting it slightly wrong is survivable (a missed
cut smears for one frame; a false cut drops one frame of temporal history). Note the model *also* has
its own `Reset` semantics; the producer's reset and the frame's reset are OR-ed, never fought over.

## 8. Cost budget and how it stays small

Target: **under ~0.5 ms** on a 4090 at 3440×1440, so the producer is noise next to the model's own pass
(measured externally at 2.1–3.4 ms depending on backend, [nr-without-game-dlss.md](nr-without-game-dlss.md)).
The budget is credible because the precedents pay far more on a *CPU*:

- NeuralScreen's whole guide stage is **3.9 ms on a moving frame, 2.9 ms of it `dis.calc`** at 320×180,
  on CPU (`TECHNICAL.md:239-240`); ultrafast DIS is 0.34 ms vs fast's 2.85 ms for the same pair
  (`guides.py:24-27`). The static-frame path is **0.03 ms** because it never calls the estimator
  (`TECHNICAL.md:236`).
- The costs that bit NeuralScreen were allocation and full-res CPU resize, not the search: it reuses
  every buffer to avoid "~11.6 ms in allocation, multiplying the already-upscaled field and astype"
  (`guides.py:86-91`) and moved the field upscale to the GPU to spare "~8 ms per frame — a resize and
  a conversion of 6 million values" (`guides.py:68-74`).

On the GPU those disappear: the search is 43k cells × a bounded per-level window, the upscale is one
bilinear pass the hardware does for free, and there is no host-visible allocation per frame. The
disciplines that keep it there:

1. **Estimate at 320-wide, never at working resolution.** Scale the vectors before the upscale
   (`guides.py:334`), which is linear-equivalent and 70× fewer elements.
2. **Skip the search on static frames.** The scene-score reduction gates the whole estimator.
3. **Allocate once per extent, reuse every frame.** Ping-pong buffers, pyramid mips and the output
   field are sized in `Ensure` and never re-created per frame — the `ZeroGuides::Ensure` pattern.
4. **Bound the search window.** Cost is `levels × cells × window² × patch²`; keep window ≤ ±4 and
   patch ≤ 7×7 and it is fixed and small. Do not "improve" quality by widening the window at full res.

**Never run the estimator's shaders through the msvc-wine compiler prefix while a DLL build is in
flight** (project CLAUDE.md, build-stall section) — the shader rebuild is a wine workload like any
other.

## 9. Open questions (honest)

1. **Does zero depth + real motion clear the above-1.0 refusal?** Unknown. Crimson supplies *both*
   real motion and real depth. If the model validates depth too, this note delivers correct motion and
   the field still refuses above 1.0, and depth reconstruction becomes a prerequisite rather than a
   later step. The cheap experiment is to run synthesized motion with the existing zero depth at 1.5×
   and read the `outReason` — `c6273373` already surfaces the size refusal distinctly.
2. **The `MvScaleY` sign, and whether the model wants +y-up or +y-down.** Estimation is image-space
   (+y down). The bridge's evidence is `uv_prev - uv_cur` in +y-down UV (`synth.inc:69-74`) but with a
   downstream `(-0.5, +0.5)` clip scale. This must be pinned by the before/after capture
   (`RequestCapture`), not asserted — a flipped sign makes motion *worsen* smearing, and would read as
   "synthesis made it worse" rather than "the sign is wrong."
3. **Is block matching's quality enough for a network trained on engine motion?** The model was trained
   on exact optical flow; block-matched flow is coarse and wrong at occlusions and thin structure.
   Whether that shows as a guide is unmeasured. The static-hypothesis guard ([§3](#the-three-passes))
   is the first mitigation; a swappable kernel is the fallback.
4. **Does the model actually inspect the texture, or only the parameters?** The present host's
   `MvScale` experiment may already answer this. If the refusal was pure parameter validation,
   synthesized motion is unnecessary at 1.0 and only earns its place in fast-camera content — which no
   title on this workstation currently reaches, so it may not be falsifiable here at all.
5. **Luma bit depth.** `R8_UNORM` previous-frame luma may starve the sub-pixel parabola fit; `R16_FLOAT`
   doubles the (tiny) storage. Decide by measuring endpoint error against a known synthetic shift, the
   way NeuralScreen scored its presets (`guides.py:29-39`).
6. **The colour the estimator reads is the model's proxy, already tonemapped-ish.** Whether luma from
   `_toWorking` (R16F proxy) or from the raw backbuffer gives steadier flow is untested; the proxy is
   chosen for being already on-device in a known state, not because it is known better.

## 10. Skeleton files in this change (unbuilt)

All under `OptiScaler/shaders/dlssnr/motion/`, none in any `.vcxproj`, none generating a precompiled
header, none referenced by existing source. Each carries a banner saying so.

- `DlssNr_SynthMotion.h` — the producer class interface: `Ensure` / `Record` / `Motion` /
  `ResetThisFrame` / `ConfirmExecuted` / `AbandonRecording` / `Release`, mirroring `ZeroGuides` and
  `PresentHost` so the integration is a small, obvious wiring change on the no-capture branch.
- `DlssNr_SynthMotion.cpp` — skeleton implementation. Method bodies are `// TODO(skeleton)` stubs that
  return "not ready" so the shape compiles in principle but the class never claims a field it did not
  produce. It is **not** wired to build.
- `precompile/synth_motion.hlsl` — the three compute entry points (luma+pyramid, coarse-to-fine block
  match, validate+scale+upscale) as documented HLSL, with the cbuffer layout. No `_Shader.h` /
  `_Shader_Vk.h` is generated; when this is built for real, both DX12 (`dxc cs_6_0`) and Vulkan
  (`dxc -spirv -D VK_MODE`) targets are regenerated, per the module's shader-rebuild invariant
  (DEVELOPMENT.md §1.6). This is Vulkan-relevant because a native Vulkan present host
  ([nr-vulkan-present-host.md](nr-vulkan-present-host.md)) would share the same producer.

These are self-contained: they include only what a compute pass needs and do not touch
`DlssNr_ZeroGuides.*`, `DlssNr_PresentHost.*`, `DlssNr_Dx12.cpp`, `Config.*`, or the vcxproj.

## 11. Step-by-step implementation plan

The order is chosen so each step is falsifiable before the next depends on it.

1. **First, settle question 4 without writing a shader.** Run the present host at 1.5× with the current
   zero field and the `MvScale` units experiment and read `outReason`. If it stops refusing, the whole
   motive weakens to fast-camera content only; record that before building anything.
2. **Build the estimator's transport with a constant field.** Wire `DlssNr_SynthMotion` into the
   present host's no-capture branch, but have `Record` emit a *hand-authored constant* motion (e.g. a
   uniform 2 px pan) into the full-res `R16G16_FLOAT` output — no estimation yet. Prove the field is
   accepted, that `MvScaleX=1`/`MvScaleY=±1` are honoured, and pin the `MvScaleY` sign with a
   before/after capture on a slow scene. This isolates the contract from the estimator.
3. **Add the luma pyramid + scene-score, still no search.** Emit zero motion but real reset/static
   decisions from the scene-score reduction. Confirm resets land on cuts and static frames cost
   nothing, using the log. This proves the ping-pong and the reset bookkeeping in isolation.
4. **Add coarse-to-fine block matching.** One level first, then the pyramid. Score endpoint error
   against a synthetic known shift (question 5's method) before trusting any game footage.
5. **Add the noise floor and the static-hypothesis guard.** In that order; the floor is one line, the
   guard is a warp+compare pass. Judge with the before/after wipe, not by eye on a single frame.
6. **Measure cost** on the 4090 at 3440×1440 against the §8 budget, static and moving, and record it in
   this note the way the other notes record their measurements. Only then is it a candidate to show a
   player, and only on the fast-camera case that motivates it.

Steps 2–5 each stay behind whatever config gate the integration worker gives the present host; nothing
here proposes a new key (one quantity, one control — the producer is on when the present host has no
captured guide, off otherwise, and needs no separate switch until a reason to turn it off appears).

## 12. Recommended first implementation step

**Step 1 — run the existing zero field at 1.5× and read `outReason`.** It costs nothing to build, it
is the one experiment that can make most of this note unnecessary, and its result decides whether the
estimator's first job is "make supersampling work at all" or "reduce fast-camera smearing." Everything
downstream is cheaper to sequence once that is known.
