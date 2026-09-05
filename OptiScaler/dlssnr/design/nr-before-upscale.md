# Optional Neural Rendering before upscaling

Status: implementation in progress; no local in-game performance or image-quality claim.

Adapted from GrimsVerk/OptiScaler_NR_then_SR, branch `nr-before-upscale`, commit
`ba3ed2e401` and its Stage=1 implementation by that fork's contributors:
https://github.com/GrimsVerk/OptiScaler_NR_then_SR/tree/ba3ed2e401
The external Jedi Survivor measurements are motivation, not validation of this port.

## Contract

`[DlssNr] Stage=0` remains the existing after-upscale path. `Stage=1` is an opt-in
D3D12 experiment: read the game's render-resolution color, compose NR into a private
texture, lend that texture to one upscaler evaluation, then restore the original
parameter value and resource states. Never write the game's input color. Frame
generation evaluations do not participate. Ray reconstruction must use the existing
after-upscale path because its input is undenoised. Native Vulkan remains after-upscale.

The decision belongs to each evaluation's scope, not a global flag: interleaved FG
must not cause both pre- and post-upscale NR to execute for one upscale. A build or
skipped NR frame must leave the original input selected. A failed upscaler call must
still restore the parameter block. Typed and untyped NGX Color slots must be preserved.

Adapt the source/target split to our GPU exposure, supersampling, reversible proxy,
and frame-hold implementation. No composition shader or cbuffer change is intended.
When NR is disabled or Stage is zero, no pre-upscale resources are allocated.

## Improvements required over the reference

- Reject unsupported formats, multisampling, nonzero color origins and invalid
  subrect bounds before recording work. Do not silently clamp an invalid contract.
- Allocate every scratch with the resource flags its requested resting state requires;
  never fall back to a texture incapable of entering that state.
- Track scratch device ownership and the exact texture borrowed by each scope.
- Debounce changing render extents before allocating/building the pre-upscale path,
  so continuously changing dynamic resolution does not rebuild the model every frame.
- On the new pre-upscale path, restore output states on every Dispatch exit and
  protect the NGX creation frame's command-list bindings. The default post-upscale
  path keeps its original behavior; its PR #23 audit findings remain a separate fix.
- Expose the experimental stage only where it can apply, and describe fallback and
  the known image-quality tradeoff without promising a particular speedup.

## Review and validation

Verify config declaration/read/save/shipped default; stage routing including FG/RR;
typed/untyped parameter restoration; no scratch substitution until a successful write;
same-sized stage switches resetting temporal history; invalid/sRGB/MSAA/subrect cases;
allocation failure and changing-resolution behavior. Build Release under msvc-wine.
Review resource ownership and every early return manually against DEVELOPMENT.md.

Automated host tests can prove routing and ownership decisions, not the NVIDIA model's
behavior. Local game A/B testing must compare fixed camera and movement at the same
settings, recording model/total GPU cost, VRAM, actual render/model dimensions, and
edge stability. Test stage toggles, resolution changes, failed/unsupported paths and
FG interleaving. No installation or marker changes are part of this implementation.
