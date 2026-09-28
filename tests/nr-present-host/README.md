# NR present host

Run `python3 tests/nr-present-host/run.py`. The runner compiles the production
`OptiScaler/dlssnr/DlssNr_PresentHost.{h,cpp}` — the bodies themselves, not a copy — against host
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

## What this does not cover

No real D3D12, NGX, shader or bridge. Whether the bridge hands the host the right frame is the
bridge's business (`tests/bridge-lifetime`); whether the model accepts it is the loopback harness's.
