# NR present host

Run `python3 tests/nr-present-host/run.py`. The runner compiles the production
`retoque/dlssnr/DlssNr_PresentHost.{h,cpp}` — the bodies themselves, not a copy — against host
fakes with ASan/UBSan. `--source <file>` compiles another copy of the `.cpp` instead, so a fix can be
shown to be what the cases detect by running them against the code from before it.

## What this covers

The present host is what runs the pass for a title that never calls an upscaler: the D3D11-to-D3D12
bridge hands it each base frame. The host decides what to build (working colour, output transfer,
zero guides), when, and on which list the model is created. The pass itself is a fake here that keeps
the one rule the host depends on (no model below `kDlssNrMinExtent`) and records every model creation
with its size and whether the list it went on was empty.

Written after PCSX2 under the D3D11 bridge (2026-09-27). The bridge handed the host a 1x1 frame
mid-session; the host released what it had built at the real size, rebuilt at 1x1 and spent the
model's creation there, and the model ran on none of the frames that followed. The cases:

- A frame below the model's minimum builds nothing, creates no list and attempts nothing; the first
  real size afterwards builds normally, with the model created on the host's own empty list.
- Running, then 1x1, then the real size again: nothing is released or rebuilt for the small frames,
  and the model runs on the first frame back.
- An ordinary resize rebuilds at the new size at once. There is no settling wait: present hosts are
  exempt from the post-upscale settling gate by design (`PresentFrameDefaults` sets
  `ExtentIsStable`), because a swapchain's extent only changes on resize and a resize rebuilds
  everything the host owns. The D3D12 present pass behaves the same way.
- Switched off, then resized: nothing is rebuilt and the model's one creation on the host's own list
  is not spent while off; switched back on, it is created there at the new size.
- A host created while the pass is off stays empty until the pass is switched on.
- With `SynthMotion` on and the model refused, the motion estimator is reset once after the build,
  not on every frame the model has no history. It carries on without a reset when the model starts,
  and a resize owes it the reset again. Otherwise it never warms up, and frame generation, which takes
  its field on the bridge, builds a second estimator (Divinity, 2026-09-28).
- An HDR swapchain (2026-09-29): a ten-bit frame the bridge was told is PQ, scRGB, HLG or Rec.2020 is
  declined before anything is built and said once; set back to sRGB it builds and runs, and a switch
  to HDR mid-session keeps what was built and owes a reset on the first frame back. FP16 is declined
  whatever colour space it was told, ten bits with none set runs (DXGI's default is sRGB), and eight
  bits run even with a PQ colour space noted. The rule is `DlssNr_PresentColour.h` itself, compiled
  in, not a fake; the D3D12 present pass calls the same function.
- HDR10 and scRGB at present (`[DlssNr] HdrPresent`, `docs/dlssnr/design/hdr-present-conversion.md`):
  - the decision, `ResolvePresentColour`: with the switch off, every format and colour space gets
    `PresentColourRefusal`'s answer, the same literal; with it on, eight bits run as SDR whatever is
    recorded, full-range PQ on ten bits is HDR10 and FP16 scRGB (set or by default) is scRGB, and studio
    or YCbCr PQ, a linear ten-bit space, HLG, Rec.2020 gamma, FP16 told anything else, a typeless format,
    no record, a resize to another format since the colour space was set and a record for another format
    are each declined with their own text; which frames reach the menu's status;
  - the D3D12 route's per-window colour record (`DlssNr_SwapchainColour.h`'s `Book`): a creation knows the
    format and no colour space, the default follows a resize until one is set, a resize to another format
    leaves it unknown -- even after changing back -- until the next `SetColorSpace1`, and a new swapchain
    on a window forgets the old one's;
  - the bridge's host: off, an HDR10 frame is declined as before and nothing HDR is built; on, it is decoded
    into the working colour, the pass is told linear HDR with the SDR white and a PQ code as the
    stabilizer's unit, and it is encoded back into the source's format with the source as the reference;
    FP16 never told a colour space converts as scRGB; a refusing model gets a decode and no encode, and its
    timing sample is closed on the open list; no shader in the build, or no typed UAV store, declines and
    says so; a format change after the colour space was set declines until it is set again; back to eight
    bits is exactly the SDR path; switched off mid-session declines with the old text.

- An sRGB swapchain (Metal: Hellsinger on native Windows, 2026-10-14): the output conversion wrote through an
  unordered-access view in `R8G8B8A8_UNORM_SRGB`, which D3D12 has no typed UAV for, and the device was removed at the
  first composition. The cases: an sRGB frame gets its output in the UNORM sibling and a flagless UNORM stage, its bytes
  are copied into the stage and the conversion in reads the stage, not the sRGB source, every frame, with no rebuild and
  one format question; a UNORM or ten-bit frame takes exactly the old path, with no stage and no copy; a resize to
  B8G8R8A8 sRGB and back builds and releases the stage; an abandoned recording rebuilds it; B8G8R8X8 sRGB, whose sibling
  has no typed store, and a device without a typed store for B8G8R8A8 are declined before anything is built and said
  once, and a resize back runs on the first frame with its reset. The fakes refuse a transfer buffer or an
  unordered-access view in an sRGB format, a copy across typeless groups, and a copy from or into the wrong state, so
  the code from before the fix fails at the first sRGB case.

  The fakes track every resource's state: a transition must start from the state the resource is in, the
  source must come back as the copy source it arrived as, and the conversion pass's fake holds each
  dispatch to the states its header documents. The conversion pass and its timing sample are fakes
  (`hdr_fakes.h`); the decision, the record and the numbers are production's.

## What this does not cover

No real D3D12, NGX, shader or bridge. Whether the bridge hands the host the right frame is the
bridge's business (`tests/bridge-lifetime`); whether the model accepts it is the loopback harness's.
The D3D12 route's own HDR wiring (`RunPresentPass`) is not compiled here; `tests/nr-hdr-math` guards its
shape by source and the shader's maths by execution.
