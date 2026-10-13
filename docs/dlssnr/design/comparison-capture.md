# Same-frame comparison capture: original and processed as image files, with provenance

Status: design for review, not implementation, 2026-10-13. Line numbers on our side are against
`93730d39` (branch `docs/design-hdr-capture`). Nothing here has been built or run.

## 1. The answer

One press (menu button, or the existing trigger file) saves **two image files of the same frame**, the frame
the pass read and the frame it produced, plus a provenance record: build id, route, model identity, and every
setting that shapes the picture. For measurement and for bug reports that need a picture a maintainer can
open without our tooling.

The decisions, each argued below:

1. **Both surfaces are copied inside the one command list that already runs the pass** (the existing record
   site, `DlssNr_Dx12.cpp:5349-5358`), so "same frame" is structural, not a matter of timing. D3D12 only
   for the first version: the after-upscale route, the D3D12 present hook and the D3D11 bridge all reach it.
2. **Readback never stalls a frame.** Copy into readback buffers on the pass's own list; remember a
   completion ticket; poll it once per frame; map and hand to a writer thread only when the GPU is proven
   done. No `WaitForSingleObject` on the render or present thread.
3. **Our own minimal writers, not WIC.** PNG (stored deflate, no compression, a `tEXt` provenance chunk)
   for display-referred surfaces; PFM (linear float) with a JSON sidecar for HDR surfaces. About 150 lines
   of dependency-free, host-testable code. Nothing in our tree writes images today.
4. **SDR first.** HDR as a linear float file, no tone-mapped preview in the first version.
5. **Zero when idle:** no allocation, no GPU work, no new config key; one relaxed atomic load per frame
   and the one-per-second trigger-file `stat` that already exists (section 4.7 says what "zero" means).
6. **It sits beside the raw burst capture that exists, not on top of it.** That one keeps its job
   (eight consecutive frames for temporal work); this one is the single matched pair.

## 2. What we already have

Verified by reading, not by running.

### 2.1 `capture::FrameCapture` (`retoque/dlssnr/DlssNr_Capture.h`)

- A burst of up to `kMaxFrames = 8` consecutive frames, "before" and "after", each a readback buffer
  sized by `GetCopyableFootprints` (`:194-221`), copied with `CopyTextureRegion` and its own barriers
  (`:223-259`). Typeless formats are mapped to typed for the footprint (`TypedForCopy`, `:171-191`); the
  comment at `:171-174` records why: an invalid copy removed the device and crashed the game every time the
  output happened to be one of those formats.
- It writes raw bytes, `before_NN.raw` / `after_NN.raw`, with a `manifest.txt` of width, height, DXGI format
  number and row pitch (`:288-336`). No image viewer opens that. The header says why: "Raw, because a codec
  is the confound" (`:12`), which is right for temporal research and wrong for a bug report.
- It discards a dark first frame and silently re-arms (`write`, `:108-118`; `isDark`, `:262-285`).
- **No fence.** `record` sets `ready_`; the caller waits eight frames (`g_captureWriteAtFrame = g_frames + 8`,
  `DlssNr_Dx12.cpp:5357-5358`, consumed at `:3969-3976`) "by which point the GPU is certainly past the
  copies". Eight is an assumption, not a proof; nothing checks it.
- **No menu button.** `DlssNr::RequestCapture` (`DlssNrFeature_Dx12.h:419`) is called from exactly one
  place: `CheckCaptureTrigger` (`DlssNr_Dx12.cpp:709-723`), which, once every 60 frames, does a
  `std::filesystem::exists` on `dlssnr-capture.trigger` beside the DLL and, if found, deletes it and
  requests a burst.
- **The output directory is wiped each run** (`ClearCaptureDirectory`, `:681-701`, `remove_all` on
  `dlssnr-capture/`). Right for a scratch burst; wrong for screenshots a user wants to keep.
- **`[DlssNr] AutoCapture` does nothing.** Declared with default `true` (`Config.h:641`), read and saved
  (`Config.cpp:457`, `:1425`), printed in the timing contract line (`DlssNr_Dx12.cpp:608`), and that is all:
  `kAutoCaptureAfterFrames = 180` and `g_autoCaptureDone` (`:677-678`) are declared and never read. A key
  that is on by default, documented as writing a session's frames "without anyone having to ask"
  (`Config.h:639-640`), and wired to nothing.
- **What "before" is.** The record site passes `sourceIsTarget ? g_nr.colorCopy : g_nr.hdrCopy`
  (`DlssNr_Dx12.cpp:5353`). `colorCopy` is the encode's `gTarget` (u0), `hdrCopy` its `gKeep` (u1): the
  `DispatchPass` call at `:4336-4337` passes `colorCopy` as `OutTarget` and `hdrCopy` as `OutKeep`, and
  `dlssnr.hlsl:744-790` writes the proxy to the first and the frame value for value to the second. So on
  the in-place routes (after-upscale and every present route, where `sourceIsTarget` is true) "before" is
  the **proxy the model was shown**, soft-knee and sRGB-encoded for a linear frame, not the frame the game
  produced. For a passthrough frame (an SDR present frame) the two are the same up to negative clamping;
  for linear HDR they are not. The comment at `:5349-5352` says "legacy", and the file header
  (`DlssNr_Capture.h:6-8`) says "the frame as the upscaler produced it". The two disagree for the
  linear case. This is a reading of the code, not a measurement; open question 4.

### 2.2 Hold frame, comparison modes, apply-model

- Hold frame (`DlssNrHoldFrame`, [frame-hold.md](frame-hold.md); menu `DlssNr_Menu.cpp:1833-1850`) freezes
  the encode's input and the white point, so one frame can be re-rendered under different settings. It is
  the natural partner of a capture: the original of two captures is the same bytes, and only the setting
  moved. v1 does not reset the model's history, so a setting change morphs over three to five frames
  (frame-hold.md, "Model history"); a capture taken immediately after a change records a half-settled
  picture. The provenance records whether the frame was held, so that is at least visible.
- `DlssNrCompare` (side by side, wipe), `DlssNrDebugView`, `DlssNrApplyModel` (output the clean frame while
  the pass still runs) are all resolved inside the same dispatch (`DlssNr_Dx12.cpp:5013-5021`). A capture
  taken while compare is on records the **composed comparison** as "processed", with the divider drawn
  into it. Section 4.5 refuses the capture in that state rather than silently saving a side-by-side as a
  result.

### 2.3 The loopback test's own readback (`tests/dlssnr-loopback`)

`harness.cpp` already reads back before and after frames and saves them as binary PPM
(`harness.cpp:929`, `P6`; names like `g3-f15-before.ppm`, README "readbacks saved"). It is test-only code,
and PPM carries no metadata and renders nowhere. Its structure is useful (record both copies on the list
that ran the pass, signal the fence after the whole recording, read back after proof of completion;
`tests/dlssnr-loopback/README.md`, "recording order") and is the template for the wine test below.

### 2.4 Image writers in the tree

None that we can use. `OptiScaler/include/imgui/imgui_draw.cpp:3975-3985` has a debug PNG writer
inside `#if 0` that includes `../stb/stb_image_write.h`, which is not in `OptiScaler/include/`
(no `stb/` directory). `external/freetype` carries its gzip support as headers only. There is no PNG, QOI or
PFM code anywhere in `retoque/`, `OptiScaler/` outside vendored code, and the menu draws through ImGui
textures, which have no write-out path.

## 3. Prior art: NeuRotic

Read-only reference (`~/.cache/neurotic-review/neurotic`; review
`~/.local/state/dlss5/notes/neurotic-20261013/report.md`, sections 3.7 and 4 item 3). GPL-3.0 like us; the
code below is ours, theirs is cited as prior art. Commits `e4cf491b` to `a9106f48`; files
`OptiScaler/dlssnr/NrScreenshotPng.h`, `NrFinalOutputCapture.h`, `NrScreenshotContract.h`,
`NrScreenshotSrPair.h`.

- **`NrScreenshotPng.h`** writes PNG through **WIC** (`#include <wincodec.h>`, `windowscodecs.lib`) and then
  *reopens the file* and inserts an `iTXt` chunk, `NeuRotic.CaptureManifest`, after the fact
  (`EmbedPngManifest`; `ReadPngManifest` verifies the chunk's CRC with a hand-written `PngCrc`). The
  container work is theirs, the pixels' encoding is WIC's.
- **`NrFinalOutputCapture.h`**: `FinalOutputSubmission`, one private submission on the swapchain's own
  queue: "the game output is copied before NeuRotic's overlay; no NR-stage texture or second game render is
  used". It keeps the allocator and list until the submission's completion is observed, and on a failed
  drain *leaks them until exit* (`retainUntilExit`) rather than free what the GPU may still read. That is the
  same rule as our DEVELOPMENT.md rule 8, applied to D3D12.
- **`NrScreenshotContract.h`**: `Identity {route, providerFrame, providerGeneration, resourceGeneration,
  backbuffer, requestedPasses, completedPasses}`, a `BackendRefusal` (needs a D3D12 swapchain, or an active
  present route on D3D11), a `JsonString` escaper.
- **HDR is an SDR preview**, both sides tone-mapped at a fixed 200-nit reference ("an explicit SDR preview
  recipe, not a monitor/HDR screenshot"). Useful for a bug report, useless for measurement.
- **`NrScreenshotSrPair.h`** runs the *upscaler* twice with reset history to make a fresh before/after pair.
  A different kind of comparison (it re-evaluates, so it is not the same frame the user saw); not wanted
  here.
- WIC is Windows-only. Whether this machine's Wine prefixes and Proton's include a working PNG encoder is
  **not measured**; Wine does build a `windowscodecs`, but nothing here depends on it, and the writer below
  does not.

What we take: both surfaces from one submission before any overlay; completion observed before release;
provenance inside the image and not only beside it; a refusal that says why. What we do not take: WIC, the
post-hoc chunk surgery, the SDR-preview-only HDR.

## 4. Design

### 4.1 Which two surfaces

| Route | Original | Processed |
|---|---|---|
| After-upscale D3D12 (`EvaluateAfterUpscale`) | `g_nr.hdrCopy` (the encode's keep, the frame value for value) | `target` after the stabilizer, in `UNORDERED_ACCESS` |
| D3D12 present hook (`RunPresentPass` through `EvaluateAtPresent`, then `Dispatch`) | the same | the same, before it is copied back to the backbuffer (`:6692-6696`) |
| D3D11 bridge (`DlssNr_PresentHost.cpp`, `Record`, then `EvaluateAtPresent`) | the same | the working buffer after the pass |
| Native Vulkan | not in v1 | not in v1 |

All three D3D12 rows pass through `Dispatch`, so one record site covers them: today's, at
`DlssNr_Dx12.cpp:5349-5358`, after the stabilizer dispatch (`:5340-5342`) and before the frame is handed
back. The Original for the pair is `hdrCopy` on every route, which is the frame the pass read (the held
frame while Hold is on, because the encode reads the held copy, `:4260` onward); it is **not** the
`sourceIsTarget ? colorCopy : hdrCopy` choice the burst makes (section 2.1). The burst keeps its choice
until the maintainer decides (open question 4).

What each surface is, so a reader of the file is not misled:

- **Present routes** (D3D12 hook, bridge): the final display-referred picture, taken inside the present
  hook before our menu overlay draws. This is "what was about to be shown" and is the surface a user can
  compare with what they saw.
- **After-upscale route**: the upscaler's output, **before the game's own tonemapper, post-processing and
  HUD**. For a linear HDR title this is scene-linear float, not what was on screen. The provenance says
  `surface=upscaler-output`.

The processed frame is **only the pass's own result**. With `DlssNrCompare != 0` or `DlssNrDebugView != 0`
the target contains a comparison or a debug picture (`DlssNr_Dx12.cpp:5013-5021`), so the capture is refused
in that state with a reason (section 4.5). It is not silently saved, and a second dispatch with compare
forced off is not added: it would double the cost of the frame being captured and would not be the frame
the user saw.

### 4.2 Readback without stalling

Reuse what `DlssNr_Capture.h` already does correctly, move it to a shared place, and fix what it assumes:

1. **Request.** A relaxed atomic flag `g_pairRequested`, set by the menu button or the trigger file. The
   menu thread allocates nothing.
2. **Record (render thread, inside `Dispatch`, under `g_nrMutex` as the burst already is).** If the flag is
   set and the route and state allow it, allocate two `D3D12_HEAP_TYPE_READBACK` buffers sized from
   `GetCopyableFootprints` of the two surfaces' typed formats, record the two `CopyTextureRegion` calls with
   the barrier pairs `copy()` already has (`DlssNr_Capture.h:223-259`), clear the flag, and note the
   **completion ticket**. Only subresource 0 is copied (as the held-frame copy does, `:4239-4258`), and a
   TYPELESS format is typed first (`TypedForCopy`), because an invalid placed footprint removes the device.
3. **Ticket.** The proof that the GPU is past the copies:
   - D3D12 present route: the per-slot present fence the pass already signals after `SubmitPresentList`
     (`:6146-6160`, `g_presentList.fenceValue[slot]`); done when
     `g_presentList.fence->GetCompletedValue() >= value`.
   - Routes where the copies ride on the game's own list or the bridge's: the `DlssNr::Submission`
     tracking the guide ring already uses (`DlssNr_Submission.h`: `Track`, `Completed`, `Abandon`), keyed
     on the list that carried them.
   - Only where neither exists, fall back to the burst's eight-frame wait and say so in the provenance
     (`completion=assumed-8-frames`). That fallback is an assumption, not a proof, and should be reachable
     by no route that has a better answer.
4. **Poll (every frame, one atomic load plus, when pending, one `GetCompletedValue`).** When the ticket is
   complete: `Map` each readback with the full read range, copy the rows out into a tight CPU buffer
   (removing `RowPitch` padding, which the burst's raw file keeps), `Unmap` with an empty written range,
   release the readback, and push one job to the writer thread. Nothing here blocks.
5. **Failure.** If completion cannot be established (device removed, timeout policy), the readbacks are
   **kept and leaked**, not released (the rule NeuRotic's `retainUntilExit` applies and ours states as
   rule 8). A capture that failed says why in the log once.
6. **One at a time.** A press while a capture is pending is ignored; the button shows "capturing"
   (`FrameCapture::request` already ignores a second request, `DlssNr_Capture.h:42-49`).

Sizes, as arithmetic: at 3440x1440 an RGBA8 surface is 19.8 MB, a FP16 surface 39.6 MB; two readbacks
(40 to 79 MB), two CPU copies, and the PNG buffers are transient and exist only between the press and the
write. At 3840x2160 double that. The two copies cost bandwidth on the frame that carries them; **the size
of that spike is not measured** and is read from frame times in the in-game run (section 6).

The writer thread does everything that is not a D3D call: format conversion, filtering, deflate framing,
CRC, file I/O, rotation. It runs at below-normal priority. It is created on the first capture and joined
at shutdown.

### 4.3 Formats

| Surface | File | Why |
|---|---|---|
| R8G8B8A8, B8G8R8A8 (UNORM, any sRGB variant) display-referred | PNG, 8-bit RGB | swizzle BGRA; alpha dropped (swapchain alpha is often garbage) |
| R10G10B10A2 display-referred (SDR ten-bit swapchain, or the bridge) | PNG, 16-bit RGB | the 10-bit code `k` becomes `(k << 6) | (k >> 4)`, which is lossless and standard |
| FP16 working colour holding a display-referred (passthrough) frame, as the bridge host does (`kWorkingFormat`, `DlssNr_PresentHost.cpp:22`) | PNG, 8-bit RGB, `round(saturate(v) * 255)` | the frame is a picture; provenance records `quantised-from=fp16` |
| FP16/FP32 linear HDR (scene-linear after-upscale, or scRGB present frames per [hdr-present-conversion.md](hdr-present-conversion.md)) | **PFM**, 3 x float32 little-endian, plus `.json` sidecar | measurement needs the numbers; a PNG would hide the range. No tone-mapped preview in v1 |
| PQ present frames (R10G10B10A2 under HDR10) | PFM of the decoded linear scRGB for both images, plus the sidecar | same; the decode is the one the HDR note defines |

Which row applies is read from what the pass already knows: `isHdrBuffer` (`DlssNr_Dx12.cpp:3938`;
passthrough frames are display-referred) and the surface's format. A format not in the table is refused
with a reason; nothing is guessed.

**The PNG writer** (about 120 lines): the 8-byte signature; `IHDR` (colour type 2, bit depth 8 or 16); one or
more `IDAT` carrying a zlib stream of **stored** deflate blocks (at most 65,535 bytes each) with filter byte
0 on every row and an Adler-32 trailer; a `tEXt` chunk `Retoque.Provenance` with the provenance line
(Latin-1, so ASCII only); for 8-bit sRGB content an `sRGB` chunk; `IEND`; a CRC-32 table per chunk.
Uncompressed is deliberate: 3440x1440x3 is 14.9 MB per file and 3840x2160 is 24.9 MB, which is acceptable for
a deliberate capture and removes the only complex part of the encoder. A compressing writer (fixed-Huffman
deflate with a small LZ77) can replace the framing later without touching the interface; it is not needed to
be correct.

**PFM** is `PF\n<w> <h>\n-1.0\n` followed by float32 rows bottom to top, little-endian (the negative scale).
It has no metadata slot, so the `.json` sidecar carries it. FP16 to float32 is converted on the writer
thread, subnormals, Inf and NaN preserved as they are (a NaN in a capture is information, not an error).

**Considered and rejected.** *QOI*: smaller and trivial, but viewer support is patchy and it has no standard
metadata slot. *PPM*: exists in the test harness and is fine there; no metadata, and a bug report that
attaches one is unreadable to most reporters. *OpenEXR*: half-float and compression are a large writer.
*WIC*: Windows-only, COM on the render thread's neighbourhood, and not host-testable; the machine this is
developed on cannot show it is present under Wine.

### 4.4 Provenance

One record, written three ways from one source: a `tEXt` chunk in the PNG, the `.json` sidecar for every
capture (PNG and PFM alike, so tooling never has to parse a PNG), and one `LOG_INFO` line. The text is
`key=value` pairs separated by `;`, values escaped for JSON in the sidecar.

Contents:

- **Build.** `VER_BUILD_COMMIT` and `VER_BUILD_DATE` (`OptiScaler/resource.h:36-48`; present only when
  `OPTISCALER_BUILD_METADATA` is defined, otherwise the record says `unstamped`; a Debug build says `Debug`).
  The menu title already shows this stamp, which is what makes a screenshot attributable
  (CLAUDE.md, "Build").
- **Route and frame.** which row of the table in section 4.1; `HookMethod`; `surface=present-final` or
  `upscaler-output`; render, work and output sizes; the format of each surface; `held` (Hold frame on or
  off); the pass's frame counters (`g_frames`, the present flip); `completion=fence|submission|assumed-8-frames`.
- **Model.** `TimingModelIdentity()` (`DlssNr_Dx12.cpp:557`: file bytes, mtime, version of the loaded model;
  its own comment says "file attributes, not a content hash"), and the NGX route and loader lines the first
  build already logs.
- **Settings.** The configuration the timing metadata already prints (`TimingMetadata`'s `contract`,
  `DlssNr_Dx12.cpp:593-613`: stage, apply, hold, white source/scale/trim, scan trim, detail, colour, max
  ratio, transfer, reversible mode, debug view, compare, downscaler, linear_hdr, compose format,
  effective transfer, enlarge). That text is built inside the timing path today; factor the string into one
  function used by both, so the contract line and the provenance cannot drift. Plus the white point the pass
  actually used (`resolveParams.WhitePoint`) and, under the HDR design, the present profile and the display
  white.
- **Hardware.** GPU name and driver version.
- **When.** A UTC timestamp, ISO 8601, and a capture sequence number.

Excluded on purpose: any file path, user name, host name, window title, environment variable, the DLL's
directory, the Steam library location, the process's command line. The executable's base name is allowed
(it is what makes a capture identifiable). The repository's `tests/repo-privacy` patterns are applied to
the **generated** text in the host test, because the tree is public and a capture attached to an issue is
as good as public too.

### 4.5 Trigger

- **Menu button** "Capture original and processed", in the Compare group next to Hold frame
  (`DlssNr_Menu.cpp:1825-1850`), the group frame-hold.md created for A/B tools. It is shown only when the
  last frame ran on a D3D12 route the capture supports, and **disabled with a reason** (the pattern the
  menu already uses for "Anchor here" when there is nothing to anchor, DEVELOPMENT.md section 2.1) when:
  compare mode or a debug view is on ("turn off compare to capture the result"); a capture is pending; the
  route is Vulkan ("not on this route yet"); a surface's format is not in the section 4.3 table; the output
  directory could not be created. DEVELOPMENT.md rule 2 (no dead control) is met by the first condition
  (absent when no D3D12 route ran) and rule 3 (one quantity, one control) by there being a single button.
  The state table gets a row and is walked in review.
- **The existing trigger file** `dlssnr-capture.trigger` (`DlssNr_Dx12.cpp:709-723`) now requests the pair, so
  a session can be asked for one from outside the game (the Linux workflow in CLAUDE.md, a test script). A
  file whose content is the word `burst` requests the old eight-frame raw burst, so that research use is
  not lost.
- **No keybinding in v1.** `ToggleKey` shows the pattern (`Config.h:309`); a `CaptureKey` is a four-point
  config change and the Hold frame keybind it would sit beside is itself still a follow-up
  (frame-hold.md, "Config / persistence"). Open question 3.
- **The dark-frame discard is not carried over.** The burst re-arms silently on a dark first frame
  (`DlssNr_Capture.h:108-118`); a user who pressed the button on a menu screen on purpose should get that
  frame. The provenance records the mean code instead, so a dark capture is labelled, not discarded.

### 4.6 Where the files go, and names

- Directory **`retoque-captures/`** beside the DLL (`Util::DllPath().remove_filename()`, as `dlssnr-capture`
  is, `DlssNr_Dx12.cpp:691`). Not `dlssnr-capture/`: that one is wiped on the first request of every run
  (`ClearCaptureDirectory`), and screenshots are meant to outlast the session. Created on first capture; a
  failure to create it is logged once and disables the button with that reason. Under Wine this is inside
  the game's directory in the prefix; the log line prints the path, and the menu shows the file name only.
- Names: `<yyyymmdd-hhmmss>-<exe>-<seq>.original.png` (`.pfm`), `<same stem>.processed.png`, `<same
  stem>.json`. A pair shares a stem; a sort by name keeps pairs together.
- A cap of **64 pairs**. On a new capture, if the count of files matching exactly this pattern exceeds it,
  the oldest *pairs* by name are deleted. Files that do not match the pattern are never touched. The cap is a
  constant, like `kMaxFrames` (`DlssNr_Capture.h:28`), so the feature cannot fill a drive (about 1.9 GB at
  3440x1440 uncompressed PNG, 3.2 GB at 4K, as arithmetic); a larger cap or a compressing writer is a later
  decision.
- Files are written under a temporary name and renamed, so a half-written PNG never sits under a final name.

### 4.7 Cost when idle

"Zero" is checked as follows:

- **Allocation: none** until the first request reaches `Dispatch`. The readbacks are allocated at record
  time and released at write time. The writer thread is created on the first capture.
- **GPU work: none.** No copy is recorded unless the request flag is set and the state allows it.
- **CPU per frame:** one relaxed atomic load in `Dispatch` for the flag, one more when a ticket is pending.
- **The trigger-file poll is not zero.** `CheckCaptureTrigger` runs a `std::filesystem::exists` once per 60
  frames (`DlssNr_Dx12.cpp:709-723`). That is existing cost, and it is the only thing that lets a session be
  asked from outside the game. It stays as it is; it is named here so "zero" is not claimed for it.
- **Config:** no new key in v1, so DEVELOPMENT.md rule 1 (default-identical) and rule 4 (round trip) are
  not engaged by this change. If `CaptureKey` is added later it is four-point and defaults to unbound.

### 4.8 D3D12 first, Vulkan later

- **D3D12:** the single record site covers the after-upscale route, the present hook and the bridge,
  because the last two reach `Dispatch` through `EvaluateAtPresent`. The bridge's final conversion back to the
  game's format (`_fromWorking`, upstream's `FT_Dx12`) is not captured; the processed image is the pass's
  working result, quantised as in the table.
- **Vulkan (later):** the native Vulkan route (`DlssNr_Vk.cpp`, `DlssNrFeature_Vk.cpp`) records
  `vkCmdCopyImageToBuffer` into a host-visible buffer on the same command buffer, with a per-submission
  fence or timeline value as the ticket. Rule 8 applies in full: drain a live device before freeing, abandon
  everything when the device is gone, never destroy a buffer the GPU may still write. Until then the menu
  button is disabled with "not on this route yet", which is NeuRotic's `BackendRefusal` in our idiom. The
  Vulkan present host ([nr-vulkan-present-host.md](nr-vulkan-present-host.md)) would inherit the D3D12
  rules by construction.

### 4.9 Relationship to the burst capture

- The burst stays: `DlssNr_Capture.h`, `RequestCapture(frames)`, `dlssnr-capture/`, raw files, the manifest.
  Its readback helpers (`Shot`, `alloc`, `copy`, `TypedForCopy`) move to a shared header used by both,
  because they encode a hard-won crash fix and must not be copied.
- Its heuristic eight-frame wait gets the same ticket where one exists (a bounded, separate change).
- `[DlssNr] AutoCapture`: wire it or remove it. This note recommends **removing** it in its own commit: a
  setting on by default that would write screen contents to disk unprompted is the wrong default for a
  feature whose point is that the user asked for the picture, it has done nothing since it was added, and a
  retired key must go through the four-point removal and the retired-identifier check. Open question 1.

## 5. Guards (DEVELOPMENT.md)

- **Rule 1, default-identical.** No allocation, GPU work or recorded command without a request; the pass's
  own commands and barriers are unchanged. The copy adds a transition pair around the surfaces it reads, as
  the burst's `copy()` does, restoring the state it found.
- **Rule 2, no dead control.** The button is absent when no supported route ran and disabled with a reason in
  each other state of section 4.5.
- **Rule 3.** One control.
- **Rule 7, passthrough.** The capture reads copies; it changes nothing the encode or resolve does.
- **Rule 8, Vulkan lifetime.** Deferred with Vulkan; D3D12 leaks rather than frees an unproven readback.
- **Review type.** Menu change: walk the state table row by row. Non-NR-path touch: confirm inert with NR
  off (the record site sits inside `Dispatch`, which does not run then).

## 6. Test plan

**Host tier** (new suite `tests/nr-capture`, registered in `tests/suites.toml`; pure functions and a
fake-device state machine, no GPU):

- PNG writer against Python's standard library: decompress the IDAT stream with `zlib`, verify every chunk
  CRC and the Adler-32, check 8-bit and 16-bit pixel exactness on random data and odd widths, and the
  `tEXt` chunk round trip (Pillow, if present, as a second reader; absent means skip, not fail).
- PFM header and row order; FP16 to float32 for 0, subnormals, Inf, NaN, negatives.
- Row-pitch removal for widths that do and do not align to 256 bytes (D3D12's pitch rule); BGRA swizzle;
  10-bit to 16-bit expansion is lossless for all 1024 codes.
- Provenance: escaping; a privacy lint applying `tests/repo-privacy`'s patterns to generated text for
  inputs that contain a home path, a Windows profile path and a tailnet address (they must not appear in
  the output).
- Naming, cap and rotation in a temp directory: deletes only matching files, oldest pairs first, never a
  foreign file; the rename-after-write.
- The state machine over a fake fence: request, record, **nothing mapped before the ticket completes**, one
  write on completion, a second request ignored while pending, a never-completing ticket leaks and does not
  release, and no call to a blocking wait anywhere on the render-thread functions (the fake counts them and
  the test asserts zero).

**Wine tier** (extend `tests/dlssnr-loopback`, `--present-nr`, serial; this is D3D12 evidence under
vkd3d-proton, not a validator):

- The fixture carries a frame counter in pixels. Run the pass with `ApplyModel` off (the pass still runs,
  output equals the clean frame), capture the pair, and assert: **both images carry frame N** (same frame,
  proven, not assumed), the original equals the fixture's bytes for frame N, and the PNG decodes to exactly
  the harness's own readback of the same frame (the existing `.ppm` pair is the cross-check).
- With the model on, assert the pair's original still equals the fixture and the processed differs from it.
- Assert the captured request produced no `WaitForSingleObject` on the host's present thread (the harness
  counts its own waits).

**In-game candidates** (D3D12 present, bridge and after-upscale each at least once; Windows RTX 3060 for the
native-D3D12 check):

- PCSX2 (`~/Games/pcsx2-nr-test/run.sh`, D3D12 present pass): the simplest SDR present capture.
- Divinity: Original Sin 2 (bridge, no upscaler).
- Crimson Desert (after-upscale; whether its frame is linear HDR and so a PFM is read from the log line
  `the game's DLSS buffer is ...`, `DlssNr_Dx12.cpp:3946`).
- With Hold frame on, two captures with one setting changed: assert the two **originals are byte-identical**
  (the property that makes the pair a measurement), and the processed images differ.
- Frame time around a capture, from the GPU timing and the game's own frame times: the cost of the two
  copies is read, not guessed.

**What "done" means.**

1. `tests/nr-capture` and `nr-invariants` pass; no config key was added, so the round-trip check is
   unchanged.
2. The wine test shows the same-frame property and writer exactness against the harness's readback.
3. A capture on each of the three D3D12 routes opens in a stock image viewer (PNG) or a numpy read (PFM),
   with its provenance in the file, the sidecar and the log.
4. Idle cost is as section 4.7 says, shown by a run with the feature unused: no allocation, no new work in
   a GPU capture (`gpu-timing.md`'s intervals unchanged).
5. No privacy-pattern match in any generated record, and `tests/repo-privacy` still passes on the tree.
6. The menu state table has its new row and was walked.

## 7. Risks

1. **A game's frame is not the surface.** On the after-upscale route the file is the upscaler's output, not
   the screen; the provenance says so, the menu help says so, and nothing hides it.
2. **Same-frame is a property of one command list.** If a route ever splits the copies across two lists or
   submissions (a future host), the guarantee is gone and the test must say so.
3. **Spike on the capture frame.** Two surface copies of tens of MB each on one frame. Unmeasured.
4. **Disk.** Uncompressed PNG is large; the 64-pair cap bounds it (arithmetic above).
5. **Typed footprint and TYPELESS.** The crash the burst already paid for (`DlssNr_Capture.h:171-174`); the
   shared helper is the mitigation and a new route must not bypass it.
6. **Wine.** Directory creation, rename-over, and a below-normal writer thread behave as on Windows; the
   writer uses only CRT file I/O and `std::filesystem`, and has no COM.
7. **Hold's history morph.** A capture a frame after a setting change is half-settled; recorded as `held`,
   not corrected.

## 8. Open questions that need the maintainer

1. **`AutoCapture`:** remove (recommended, own commit) or wire to one automatic pair?
2. **Compressing PNG writer now or later?** Uncompressed is proposed, with the 64-pair cap.
3. **A `CaptureKey`** in v1, or button plus trigger file only?
4. **The burst's "before"** is the encode's proxy on in-place routes; change it to the frame itself
   (`hdrCopy`) as the pair does, or leave the burst's historical choice?
5. **HDR preview.** PFM only in v1; do you want a fixed-white SDR preview PNG beside it for bug reports
   (NeuRotic's choice, 200 nits there)?
6. **Provenance disclosures:** GPU name and driver version are included; confirm that is acceptable for
   files users will attach to public issues.
7. **Directory name** `retoque-captures/` and the 64-pair cap.
