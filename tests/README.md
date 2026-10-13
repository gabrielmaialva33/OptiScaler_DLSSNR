# Tests

Every suite is a self-contained directory with its own `run.py` and `README.md`.
The host suites are not a unit-test framework: each host `run.py` slices production source out
of `OptiScaler/`, compiles that slice against local fakes, runs it under
sanitizers, and asserts. That is deliberate, and it is also the reason the suites
are honest about what they do not prove.

## Running them

```bash
python3 tests/run_all.py            # the host tier, the default
python3 tests/run_all.py --list     # registry with tiers and availability
python3 tests/run_all.py --tier all # host and wine
python3 tests/run_all.py --only nr-menu,nr-multipass -v
python3 tests/<name>/run.py         # one suite, unchanged, no driver involved
```

`run_all.py` never replaces a suite. It reads `suites.toml`, decides what can run
on this machine, runs the host tier in parallel and the wine tier serially, and
prints one summary. A missing toolchain is a skip with a reason, not a failure.

`suites.toml` is also a drift guard. A directory with no registry entry, or a
registry entry with no directory, fails the run. Add a suite and you add its
entry, or the next run tells you.

## Tiers

| Tier | Needs | Suites |
|---|---|---|
| `host` | Python, `g++`, `clang++` | the `nr-*` suites, `bridge-lifetime`, `bridge-window`, `vulkan-query-readiness`, `mfg-pattern`, `fg-synth-policy`, `sl-log-repeat`, `repo-privacy` and `lsfg-shader-library` |
| `wine` | msvc-wine prefix; `vulkan-overlay` also needs a graphical session and a working Vulkan loader | `nr-gpu-timing-d3d12`, `synth-motion-d3d12`, `vulkan-overlay`, `dlssnr-loopback` |
| `wip` | registered, no runner yet | — |

The wine tier is serial on purpose. These suites compile through the single msvc-wine
prefix, which is also what `build-local.sh` uses, so do not start one while a
build is running. `CLAUDE.md` has the stall signature and the recovery.

## What the suites cover

**Host tier.** Forty-two suites. All but `nr-invariants` and `repo-privacy` build real production code with local fakes under
AddressSanitizer and UndefinedBehaviorSanitizer.

- `lsfg-shader-library` — the LSFG runtime shader library (`retoque/lsfg/LsfgShaderLibrary.{h,cpp}`): DLL discovery (Windows Steam `libraryfolders.vdf`, registry, Wine/Proton `Z:` paths, `$HOME`, `$XDG_DATA_HOME`, override, `Lossless.dll` folder mapping), PE `RT_RCDATA` SPIR-V module extraction (99 modules), SPIR-V magic validation, Quality/Performance and FP32/FP16 resource ID resolution, real user DLL extraction when present, and content-hash-keyed DXIL cache storage/invalidation.
- `nr-kernel-profile` — DLSS-NR NvAPI CUDA kernel classification by prefix (pre_block, swin_*, vit,
  etc.), FP8 variant identification, and per-window timing aggregation (mean and maximum).
- `nr-detail-measure` — the "Measure detail" CPU tile reduction and statistical aggregation (detail added,
  Laplacian raw, flicker beyond native, OkLab chroma/warmth, and shadow darkening/crush), delta comparisons
  and textual metric generation.
- `nr-session-guard` — the NR session guard (`DlssNr_SessionGuard.h`): what a marker left by an unclean session
  means (early end disables and stays blocked until acknowledged, middle ends warn then disable on the second,
  long ends are not blamed), who owns a marker (live second instance, reused pid), unreadable and read-only
  cases, and the call-site and session-only-disable source guards. The Windows layer is not exercised.
- `nr-residual-limit` — the residual limit (`docs/dlssnr/design/residual-limit.md`): the composition's guard
  and limit sliced out of `dlssnr.hlsl` and compiled against HLSL's intrinsics, held against the guard alone (off
  is the guard to the bit; on bounds both ways in stops, only narrows, stays inside the guard, symmetric); the
  host's clamp; source guards on where the constant is read, set, logged and shown. No GPU, no shader compile.
- `nr-pulse-metric` — the pulse metric (`docs/dlssnr/design/pulse-metric.md`): the edit's light from the GPU's
  grid, consecutive frames paired into light steps by kind (model, carried, every frame) with holes, gaps,
  late and non-finite readbacks breaking the pair, two-second windows, reset, the menu's words, the log line and
  its rate, and a synthetic cadence's pulse. Source guards on the pass and the renderer: off builds and records
  nothing, one sample per present. No GPU, no shader.
- `nr-present-sync` — what `[DlssNr] PresentSync=auto` means: the D3D12 present pass waits for its write-back
  only when frame generation could be involved (`DlssNr_PresentSync.h`). The wait itself is not exercised.
- `nr-dx11-route` — a D3D11 game's DLSS with NR on (`docs/dlssnr/design/dx11-native-dlss-guides.md`, steps 1
  and 2): `dlss_12` chosen over native D3D11 DLSS when `Dx11Upscaler` is unset, an explicit key kept, native DLSS
  reported once in the log and shown in the NR panel while it runs (`DlssNr_Dx11Route.h`, and the production layer
  `DlssNr_Dx11Upscaler.h` against a Config stub); source guards on the two call sites in `NVNGX_DLSS_Dx11.cpp` and
  the panel. No D3D11 device, NGX or DLSS.
- `nr-present-guides` — selection and completion tracking for the present-time guide snapshot ring
  (`DlssNr_PresentGuideRing.h`), preventing cross-queue GPU waits and DLSS-G deadlocks.
- `nr-present-guide-identity` — step 0 of `docs/dlssnr/design/present-real-guides.md`, log only: the identity
  decision (`DlssNr_PresentGuideIdentity.h`) rule by rule and in its first-failing order, that it only ever narrows the
  ring's pick, the null swapchain stamp, ring scenarios (frame generation, a late copy, portraits, a resize, a second
  swapchain), the per-window record and its line and rate, and the renderer's glue sliced out and run against fakes.
  Source guards hold that the present pass and the bridge still read the slot `PickForPresent` gave them. No GPU.
- `bridge-lifetime` — production D3D11/D3D12 bridge waits, failed submission, resize,
  ownership and deferred release against scripted COM/Win32 fakes, and which FG input and output
  the bridge is built for. Also a refused presenter resize and its recovery, and the D3D12 overlay's
  render-target release under the present-thread race that caused it. Also a second swapchain taking the
  window over: the old wrapper's surrender, and its `Present`, resizes, fullscreen and release afterwards.
  No GPU coverage.
- `bridge-window` — the takeover's device-free header (`retoque/with_dx12/Dx11wDx12_Window.h`): live bridges
  by window, newest first, with a count at zero never revived and `acquire` run under the lock `Remove` takes.
  Also the presenter gate, against a race of calling threads, and the sequence: only on E_ACCESSDENIED, one
  release, one retry. No D3D11, D3D12 or DXGI.
- `vulkan-query-readiness` — production Vulkan GPU timer (`GpuTime_Vk`, which upstream put under
  every `Shader_Vk`, NR's included, in place of the deleted `UpscalerTime_Vk`): its three-pair ring,
  nonblocking readiness retry, no publication on error, single consumption. No frame-identity or GPU
  synchronization coverage.
- `mfg-pattern` — the real MFG byte signatures and scanner matcher against synthetic
  buffers. Runs no patcher and touches no DLL.
- `sl-log-repeat` — the Streamline log repeat filter against the real `setDynamicMFGParams` line
  on a fake clock: one line per 10 s with the folded count, new sites at once, a bounded table that
  fails open. Runs no Streamline and no callback.
- `repo-privacy` — every tracked file outside `external/` and `OptiScaler/include/` is free of home
  paths, LAN or tailnet addresses and tokens; each pattern is self-tested against a sample and a near
  miss. Pure Python. A line that must keep a match says `privacy: allow`.
- `fg-synth-policy` — synthesized FG's decisions (fast-motion response on incoherent motion
  and its cap, low-fps floor, duplicate-present advice, which HUD fix runs for which output), the
  motion statistics they are made from, and the NR/FG motion handoff, from the production headers,
  plus a guard holding the HUD mask's thresholds equal to DLSS-NR's. Pure logic on rows laid out as
  the readback lays them out: no GPU, no FSR, no DLSS-G. Also the UI layer's own mask on the CPU, the shader's
  header over fixtures: a glyph and a 1 px frame over a pan are taken whole, and a swaying silhouette, a moving
  patch, rain and a straight edge along a pan are not.
- `nr-before-upscale` — pre-upscale boundary functions against strict host fakes.
- `nr-capture` — the comparison capture's pure half (`docs/dlssnr/design/comparison-capture.md`): the PNG writer
  (stored deflate, CRC-32, Adler-32, the provenance tEXt chunk) and PFM, decoded with Python's `zlib` and checked
  pixel by pixel against readback-shaped sources with padding, every half and R11G11B10 value, the
  request/record/ticket/hand-off state machine over a fake backend (nothing read before completion, nothing freed
  before it is proven safe), the menu button's table, names and the 64-pair cap, write-then-rename, the record in its
  three forms with the repository's privacy patterns run over it, and source guards on the pass. No GPU.
- `nr-cadence` — model cadence (`docs/dlssnr/design/model-cadence.md`): the production scheduler (age
  in presents the pass ran at, the held reset, forced model frames, every refusal with its reason, off
  allocating nothing) and the production per-pixel rules -- the header the shader includes -- over
  synthetic sequences: a still scene carried exactly, integer and sub-pixel pans, an occluder
  restarting the chain, no match no fill, UI protection. Smoothing (`docs/dlssnr/design/cadence-smoothing.md`):
  the walk's weights and per-frame decisions, weight 0 equal to the carry to the bit, a drift arriving in equal
  steps and moved with the surface, disocclusions and the interface taking the answer at once, the light
  limiter's rule, space and rate. Source guards on the pass. No GPU, no shader.
- `nr-dispatch` — composition constants, descriptor slots and partial initialization cleanup.
- `nr-enlarge` — Transfer 2, the model's edit enlarged by a private DLSS Super Resolution: the
  decision table (what the resolve is sent and why, supersampling and Vulkan included), the carrier
  through FP16, the NGX adapter's parameters and release order against a recording core, and
  production's plan, build, evaluate, retirement and Retry, sliced out of the renderer and run against
  D3D12 fakes that chain every barrier. No GPU, NGX or shader.
- `nr-gpu-timing` — portable timing validation, plus the duplicate-execution
  guard at the submission boundary, and the HDR present conversion's own sample (its decode and encode,
  outside the Dispatch interval).
- `nr-gpu-timing-config` — the Config GPU-timing transactions and their INI
  expressions.
- `nr-invariants` — the `DEVELOPMENT.md` §4 guards against the live tree: config
  round-trip for every `DlssNr` key, struct equals cbuffer, precompiled headers equal
  the committed bytecode, retired identifiers. Pure Python, no compiler.
- `nr-localization` — the real portable `.lang` parser and the repository ImGui
  hash; label identities and collisions under translation, and a drift guard that every
  menu string routed through the translator has a pt-BR entry. Slowest suite, around 45 seconds.
- `nr-log-rate` — the log rate limiter against injected time points, a recorded
  session replay, and the four real report classifications/initializers extracted
  from the renderer, including manual configuration changes and reversals.
- `nr-menu` — pass-menu control flow driven by scripted ImGui events.
- `nr-model-loader` — what the model loaders decide from bytes: the model's caller-path import
  slots (for `ModelLoader=direct`) and the NGX loader's feature-18 table (the 616.64 route), against
  synthetic PE64 images and, when present, this machine's real `_nvngx.dll` and model; the 616.64
  faulting-pairing predicate; and the direct runtime held to the forwarder's writes. Runs no model.
- `nr-multipass` — portable multi-pass chain helpers.
- `nr-pass-config` — the pass-settings codec and the four-point Config round trip
  for the master keys.
- `nr-periphery` — peripheral compression's layout and per-pixel mapping, compiled from the header the
  shader includes: extents and stated refusals, the inverse, monotonicity and no kink at either join,
  the whole-texel centre at 100, the uniform path it must reduce to (the guide read and the downsample),
  the colour pack's weights and conservation, and motion converted in frame pixels (the defect in
  wilsjo2's v0.8.91 pinned). Also holds its constant block to the cbuffer. No GPU, no model.
- `nr-present-host` — what the production present host builds and when: a frame too small
  for the model, a resize, the pass switched off, an HDR swapchain declined, and on which list the
  model is created. With `[DlssNr] HdrPresent`, the HDR decision table, the D3D12 route's per-window
  colour record and the bridge's decode and encode around the pass, every resource state tracked.
- `nr-hdr-math` — the HDR present conversion (`docs/dlssnr/design/hdr-present-conversion.md`): the
  shader's own maths sliced out of `dlssnr_present_hdr.hlsl` and held to a float64 reference -- every PQ
  code round-trips, untouched HDR10 and scRGB texels come back bit for bit through FP16 and the
  composition's white point, an edit lands within one code -- its constants and matrices against the
  BT.709/BT.2020 primaries, and source guards on the pass class and both hosts. No GPU, no shader.
- `nr-uimask-rule` — the HUD-protection mask's per-pixel rule, compiled from the header the shader
  itself includes, over synthetic sequences: a glyph over a panning scene is protected after the
  streak and nothing else is; still, grainy scenery beside a swaying silhouette is never protected
  (the rim the first slice left in PCSX2); a vanished glyph loses its protection at once.
- `nr-shutdown` — the private NGX core shutdown adapter and its public calling contract.
- `nr-submission` — the command-list submission lifetime model that gates
  resource release.
- `nr-timing-boundary` — timing metadata and UI boundary, and where the timing
  markers sit.
- `nr-vk-extensions` — the Vulkan device-extension merge the NR hook hands `vkCreateDevice`,
  compiled against the real Vulkan headers: `VK_KHR_` and `VK_EXT_buffer_device_address` never
  leave it together, and the one the game asked for is the one kept.
- `nr-zero-guides` — zero depth and motion for a host with no engine buffers, and the
  two-heap clear contract.

**Wine tier.**

- `nr-gpu-timing-d3d12` — real D3D12 timestamp queries in an isolated prefix. It
  never loads NGX and never launches a game.
- `dlssnr-loopback` — drives the production NGX exports, NR composition, resolution changes
  and shutdown on a real GPU under Proton.
- `synth-motion-d3d12` — the synthesized-motion estimator on a real D3D12 device: endpoint
  error against known motion, the axis signs, scene cuts, abandon semantics and GPU time; and
  synthesized FG's HUD mask on the same frames (no scenery marked, precision and recall on the
  overlay, its depth and UI layer exact).
  `--source nvofa` runs the NVIDIA Optical Flow source through the same sequences, and skips
  when `nvofapi64.dll` or its shaders are unavailable. No NGX, no game.
- `vulkan-overlay` — builds a real DLL and drives real Vulkan to exercise overlay
  lifetime.

**Not a suite.** `nr-before-upscale/verify-invariants.py` is a one-shot evidence
script pinned to baseline commit `660303ec`. `run.py` does not call it and
`run_all.py` does not run it. Invoke it by hand when you want that comparison. It
asserts the shader is unchanged since that baseline, so it fails after any shader
edit by design; `nr-invariants` carries its general checks against the live tree.

## What none of this covers

The host `nr-*` suites exercise decision logic against fakes. They do not execute NGX,
a real D3D12 device, or a real shader. A green run says nothing about GPU
behaviour, image quality, or performance. In-game validation is still required;
`CLAUDE.md` has the test target. For Neural Rendering changes, work through the
per-change-type review in `docs/dlssnr/design/DEVELOPMENT.md` §3 before
calling a change done.

## Adding a suite

1. Create `tests/<name>/` with `run.py` and `README.md`.
2. Register it in `suites.toml` with a tier, a timeout and a one-line summary.
3. `run.py` must exit non-zero on failure and take no required arguments.
4. Build into a `tempfile.TemporaryDirectory`, or into `tests/<name>/artifacts/`
   if you want the intermediates to survive for inspection. The root
   `.gitignore` already ignores `artifacts/` and `__pycache__/` everywhere; a
   per-suite `.gitignore` for those is redundant.
5. Prefer `g++`, `-std=c++20`, `-fsanitize=address,undefined`, `-Wall -Wextra
   -Werror`. Three host suites predate that
   convention and omit `-Werror`: `nr-before-upscale`, `nr-localization`, `nr-menu`.

### On the `stubs/` directories

`nr-gpu-timing`, `nr-gpu-timing-d3d12` and `nr-localization` each carry their own
`stubs/Logger.h` and `stubs/pch.h`, and no two are the same file. That is not an
oversight to consolidate: each stub is the minimum that suite's translation unit
needs, and shrinking a stub is how a suite proves it does not depend on more.
Write a new minimal stub for a new suite rather than reaching for a shared one.
