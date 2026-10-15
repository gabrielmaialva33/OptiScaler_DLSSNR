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
| `host` | Python, `g++`, `clang++` | the `nr-*` suites, `bridge-lifetime`, `bridge-window`, `vulkan-query-readiness`, `mfg-pattern`, `mfg-patch-plan`, `mfg-probe`, `fg-synth-policy`, `sl-log-repeat`, `repo-privacy`, `deploy-safety`, `windows-installer`, `lsfg-shader-library`, `lsfg-integration` and `lsfg-presenter` |
| `wine` | msvc-wine prefix; `vulkan-overlay` and `vulkan-present-host` also need a graphical session, and they and `synth-motion-vk` a working Vulkan loader | `nr-gpu-timing-d3d12`, `nr-session-exit`, `synth-motion-d3d12`, `synth-motion-vk`, `vulkan-overlay`, `vulkan-present-host`, `dlssnr-loopback`, `nr-present-guide-identity-msvc`, `nr-hdr-d3d12`, `dx11-guides-transport` |
| `wip` | registered, no runner yet | — |

The wine tier is serial on purpose. These suites compile through the single msvc-wine
prefix, which is also what `build-local.sh` uses, so do not start one while a
build is running. `CLAUDE.md` has the stall signature and the recovery.

## What the suites cover

**Host tier.** Fifty-two suites. All but `nr-invariants`, `nr-ab-compare`, `repo-privacy`, `deploy-safety` and `windows-installer` build real production
code with local fakes under AddressSanitizer and UndefinedBehaviorSanitizer.

- `nr-ab-compare` — stdlib offline Stage 0/1 capture analysis: synthetic PNG/PFM/raw pairs, detail/noise/colour/temporal controls, provenance and invalid-input rejection. No GPU or image-quality claim.

- `lsfg-shader-library` — the LSFG runtime shader library (`retoque/lsfg/LsfgShaderLibrary.{h,cpp}`): DLL discovery (Windows Steam `libraryfolders.vdf`, registry, Wine/Proton `Z:` paths, `$HOME`, `$XDG_DATA_HOME`, override, `Lossless.dll` folder mapping), PE `RT_RCDATA` SPIR-V module extraction (99 modules), SPIR-V magic validation, Quality/Performance and FP32/FP16 resource ID resolution, real user DLL extraction when present, and content-hash-keyed DXIL cache storage/invalidation; plus `tools/lsfg-study/convert.py`'s Float16 detection (synthetic modules, and exactly ids 51..98 on the real DLL).
- `lsfg-integration` — LSFG 3.1 as FSR-FG's interpolator (study branch, `retoque/lsfg/`): the DXIL PSV0 reader
  the root signatures come from (synthetic containers, every truncation and byte corruption), the pass graph
  (34 + 66 dispatches, extents against `tools/lsfg-study/PIPELINE_V3_1.md`, pools, one writer per image,
  variants, flow scale), the cbuffer table, which frames LSFG refuses, the warm-up/reset schedule, the `[LSFG]`
  four-point round trip and the `FSRFG_Dx12.cpp` call sites as a set. With `LSFG_DXIL_DIR`, the user's own 99
  converted modules against the graph. No D3D12 is executed.
- `lsfg-presenter` — LSFG at x3 and x4 through the fork of FFX's swapchain (study branch,
  `retoque/lsfg/presenter/`, `tools/lsfg-study/MFG_PRESENTER.md`): the pacing arithmetic in `MfgPacing.h` (x2's
  present delta equal to FFX's formula, x3/x4 splits, output-set double buffering, a steady-state schedule, the
  10 s summary), and source guards: FSRFG's swapchain calls routed, our swapchain created only behind the latch,
  the router forwarding everything else to `FfxApiProxy` unchanged, no FFX interface on the fork, nothing freed
  before the drain, AMD's MIT notice on every copied file and the SDK's bytecode verbatim. No D3D12 is executed.
- `nr-kernel-profile` — DLSS-NR NvAPI CUDA kernel classification by prefix (pre_block, swin_*, vit,
  etc.), FP8 variant identification, and per-window timing aggregation (mean and maximum).
- `nr-detail-measure` — the "Measure detail" CPU tile reduction and statistical aggregation (detail added,
  Laplacian raw, flicker beyond native, OkLab chroma/warmth, and shadow darkening/crush), delta comparisons
  and textual metric generation.
- `nr-session-guard` — the NR session guard (`DlssNr_SessionGuard.h`): what a marker left by the last session
  means, by how it ended (a normal quit leaves none; an exception or a device NR saw lost disables under 10 minutes;
  an exit code or no end seen disables under 2 minutes and warns up to 10, the second in a row disabling), who owns a
  marker (live second instance, reused pid, the probe's answers), unreadable, other-format and read-only cases, the
  end of the process (fixed buffer, bounded lock tries, nothing started after it), the exact menu and log texts, and
  source guards on the call sites, the exit hook, the at-exit code paths and the project entries. The Windows file
  I/O is not exercised; the hook is, by `nr-session-exit`.
- `nr-residual-limit` — the residual limit (`docs/dlssnr/design/residual-limit.md`): the composition's guard
  and limit sliced out of `dlssnr.hlsl` and compiled against HLSL's intrinsics, held against the guard alone (off
  is the guard to the bit; on bounds both ways in stops, only narrows, stays inside the guard, symmetric); the
  host's clamp; source guards on where the constant is read, set, logged and shown. No GPU, no shader compile.
- `nr-pulse-metric` — the pulse metric (`docs/dlssnr/design/pulse-metric.md`): the edit's light from the GPU's
  grid, consecutive frames paired into light steps by kind (model, carried, every frame) with holes, gaps,
  late and non-finite readbacks breaking the pair, two-second windows, reset, the menu's words, the log line and
  its rate, and a synthetic cadence's pulse. Source guards on the pass and the renderer: off builds and records
  nothing, one sample per present. No GPU, no shader.
- `nr-present-boundary` — the present pass under the game's own DLSS-G (`DlssNr_PresentBoundary.h`):
  `HookMethod=2` declines on the wrapped entry below DLSS-G, Auto stays upscaled for the session once it has seen
  it, `HookMethod=1` and OptiScaler's own frame generation are untouched; source guards that `RunPresentPass` and
  the upscaled route decide through it. Not whether `State` reflects the game's DLSS-G in a given title.
- `nr-present-sync` — what `[DlssNr] PresentSync=auto` means: the D3D12 present pass waits for its write-back
  only when frame generation could be involved (`DlssNr_PresentSync.h`). The wait itself is not exercised.
- `nr-dx11-route` — a D3D11 game's DLSS with NR on (`docs/dlssnr/design/dx11-native-dlss-guides.md`, steps 1
  and 2): `dlss_12` chosen over native D3D11 DLSS when `Dx11Upscaler` is unset, an explicit key kept, native DLSS
  reported once in the log and shown in the NR panel while it runs (`DlssNr_Dx11Route.h`, and the production layer
  `DlssNr_Dx11Upscaler.h` against a Config stub); source guards on the two call sites in `NVNGX_DLSS_Dx11.cpp` and
  the panel. Since step 3, the route line at HookMethod 2 only while the native guides are not carried, and each
  evaluate handed to the transport. No D3D11 device, NGX or DLSS.
- `nr-dx11-native-guides` — step 3 of the same note, option (c): a native D3D11 DLSS evaluate's depth and motion
  carried through the bridge to the present pass (`DlssNr_Dx11Guides.h`): every gate and refusal in order, another
  view of the game left out, the slot rotation against a simulated GPU (no set written while a list reads it), the
  window and its line; source guards on the transport (`DlssNr_Dx11Guides.cpp`), its one call site, the capture core
  it shares with the D3D12 route, the present host's half and shutdown. No D3D11, D3D12 or GPU.
- `nr-present-guides` — selection and completion tracking for the present-time guide snapshot ring
  (`DlssNr_PresentGuideRing.h`), preventing cross-queue GPU waits and DLSS-G deadlocks.
- `nr-present-guide-identity` — step 1 of `docs/dlssnr/design/present-real-guides.md`: the identity decision
  (`DlssNr_PresentGuideIdentity.h`) rule by rule and in its first-failing order, rule 6 enforced and not, that it only
  ever narrows the ring's pick, the null swapchain stamp, ring scenarios (frame generation, a late copy, the wrapped
  entry's lag, portraits, a resize, a second swapchain), a long run that reads only what the rules accept and never
  overwrites a read in flight, the per-window record and its line and rate, the source-change reset, the status and
  the menu's guides line, and the renderer's glue sliced out and run against fakes. Source guards hold that the present
  pass and the bridge read only the decision's slot. No GPU.
- `bridge-lifetime` — production D3D11/D3D12 bridge waits, failed submission, resize,
  ownership and deferred release against scripted COM/Win32 fakes, and which FG input and output
  the bridge is built for. Also a refused presenter resize and its recovery, the D3D12 overlay's
  render-target release under the present-thread race that caused it, and what a flip-model presenter needs
  on native Windows (two buffers at least, its overlay let go on an NR-only bridge). Also a second swapchain taking the
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
  buffers, both wrapper clamp shapes (`r8d`, and the `edx` one of Streamline 2.7.3 to 2.14), and the
  foreign-gate patterns that recognise another unlocker's rewrite. Runs no patcher and touches no DLL.
- `mfg-patch-plan` — the MFG unlock's write half (`retoque/framegen/dlssg/MfgPatchPlan.h`) over a fake
  module whose writes can be told to fail: a patch group commits whole or rolls back whole, a half-written
  site is restored, a wrong read-back rolls back, a site changed since planning writes nothing, and a failed
  restore is reported without stopping the others.
- `mfg-probe` — the step-0 MFG-below-Ada probe logic (`retoque/framegen/dlssg/MfgProbe.h`, std-only): the
  gate exits before the claim, foreign-loader check, census, facts or emit off `[Turing, Ada)` (Ada,
  Blackwell, an NVIDIA adapter whose architecture was never read, Volta, non-NVIDIA), counted through an
  injected environment that also carries the production glue's stateful steps, plus the production gate
  memo (`GateCache`); the fatbin/cubin census over synthetic containers (SASS sm_86, sm_89/sm_120-only,
  PTX-only, bare cubins, an ELF inside a container, truncated and 2^64-wrapping lengths); CUDA sm_86
  loadability; the exact text of the module and admission lines and each NGX skip reason; and foreign-loader
  detection. With Crimson Desert's `nvngx_dlssg.dll` present it censuses the real provider against the
  offline inventory (70 containers, frame 0/31, 39 sm_86 cubins), read in place and never copied; that case
  skips when the file is absent. Writes nothing and runs no patcher or DLL.
- `sl-log-repeat` — the Streamline log repeat filter against the real `setDynamicMFGParams` line
  on a fake clock: one line per 10 s with the folded count, new sites at once, a bounded table that
  fails open. Runs no Streamline and no callback.
- `repo-privacy` — every tracked file outside `external/` and `OptiScaler/include/` is free of home
  paths, LAN or tailnet addresses and tokens; each pattern is self-tested against a sample and a near
  miss. Pure Python. A line that must keep a match says `privacy: allow`.
- `deploy-safety` — `tools/deploy-to-game.sh` on fake game directories: refuses while a process maps a file
  there (a Python child mmapping a fake DLL stands in for the game), on a foreign or case-variant DLL and on a
  second OptiScaler proxy name; Keep-ReShade renames and sets `[Plugins] LoadReshade=true` by replacing lines,
  never adding a second; the receipt and its partial form after an injected failure; dry runs write nothing;
  `--uninstall` returns the directory to its snapshot before the deploy. Python and bash, no Wine.
- `windows-installer` — `tools/install-windows.ps1`, the same contract for Windows users, on fake game folders
  in a pinned PowerShell 7.4 container (streamed in with tar, since the docker context may be remote) or a local
  `pwsh`: the same refusals, Keep-ReShade, receipt, partial receipt, dry runs and uninstall; receipts written by
  `deploy-to-game.sh` uninstalled by it and the reverse, and both scripts writing the same receipt and files for
  the same folder; `nvngx_dlssnr.dll` never touched. The running-game checks run through Linux stand-ins (a
  process started from the folder, a `flock`-emulated exclusive handle). Static checks (ASCII, the `.cmd`'s CRLF,
  the packager's allow-list) run always; without docker or `pwsh` the cases print `SKIP` and the suite passes on
  those. Nothing here runs Windows, Windows PowerShell 5.1 or the `.cmd`.
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
  before it is proven safe), the menu button's table and its order, the record site's refusal order, names and the
  64-pair cap applied after the write, write-then-rename with the failing step named, the folder in a real read-only
  directory, the record's exact text from the production builders and in its three forms with the repository's
  privacy patterns run over it, and source guards on the pass. No GPU.
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
- `nr-vk-present-host` — the Vulkan present host's stages 1 to 5 (`docs/dlssnr/design/nr-vulkan-present-host.md`):
  the registry header (`DlssNr_VkPresentRegistry.h`) through its state machine -- records with ids of their own, a
  reused handle as a new record, two devices with the same swapchain handle value, destroys taking their children,
  unknown and internal objects, the window, creation and destruction lines field for field -- and the transport's
  decisions (`DlssNr_VkPresentTransport.h`): the usage plan at swapchain creation and every refusal's reason, the
  route at present (same queue, cross-family, concurrent, none), the ring, the counters. Source guards: one call site,
  first in `VulkanHooks::Hook` and ahead of the marker check, gated before anything else; only an eligible swapchain's
  usage and a transported present's waits ever edited; no refusal after a wait is spent; locks in order; drains
  before frees. Stage 3: the colour plan, the device's extensions and bufferDeviceAddress, the model's counters and
  lines, the one-owner arbiter; guards that a model frame is decided before recording, counted on submit, seen
  complete on a fence, freed only idle and abandoned on a lost device. Stage 4 (`DlssNr_VkPresentFg.h`): the
  frame-generation shape from the state the D3D12 route reads and the FG output's own facts, the carry decided for
  each (untouched, or the round trip without NR in the pacer experiment), the unsupported line's text, Streamline's
  log lines sorted into fence timeouts, pacer lines, warnings and errors, and the pacer trace's windows, percentiles
  and flushes; guards that the frame-generation branch comes before housekeeping and before any edit, that the trace
  is off unless its key is on, that its locks are leaves, and the Streamline call sites. No Vulkan or Wine.
- `nr-zero-guides` — zero depth and motion for a host with no engine buffers, and the
  two-heap clear contract.

**Wine tier.**

- `dx11-guides-transport` — the native D3D11 guide transport's API sequence (`dx11-native-dlss-guides.md`, step 3)
  on real devices under a local Proton, DXVK and vkd3d-proton as a game gets them: depth and motion formats cross
  through shared textures and a shared fence into ring-style clones, six frames each, read back byte for byte. It
  also takes apart the hazard the transport is built around: D3D12 never sees a D3D11 copy into a shared texture that
  keeps the depth-stencil bind, unless D3D11 accesses it again before the hand-off. No OptiScaler, NGX or game; SKIP
  without a local Proton.
- `nr-session-exit` — the session guard's exit hook (`DlssNr_ProcessEnd.cpp`, built as shipped) in a real process
  under the system Wine and the newest GE-Proton's: every way a process ends itself reaches the hook once, with the
  status the design note predicts and the other threads still running; a process killed from outside, and
  `abort()`'s fail-fast, record nothing. Never loads OptiScaler.dll, NGX or a game.
- `nr-gpu-timing-d3d12` — real D3D12 timestamp queries in an isolated prefix. It
  never loads NGX and never launches a game.
- `nr-present-guide-identity-msvc` — `nr-present-guide-identity`'s decision cases and its glue slice, compiled by
  MSVC in a private copy of the prefix and run under Wine: the same checks through the compiler and standard library
  the DLL ships with. No D3D12.
- `dlssnr-loopback` — drives the production NGX exports, NR composition, resolution changes
  and shutdown on a real GPU under Proton.
- `synth-motion-d3d12` — the synthesized-motion estimator on a real D3D12 device: endpoint
  error against known motion, the axis signs, scene cuts, abandon semantics and GPU time; and
  synthesized FG's HUD mask on the same frames (no scenery marked, precision and recall on the
  overlay, its depth and UI layer exact).
  `--source nvofa` runs the NVIDIA Optical Flow source through the same sequences, and skips
  when `nvofapi64.dll` or its shaders are unavailable. No NGX, no game.
- `synth-motion-vk` — the same estimator on Vulkan (`SynthMotion_Vk`, its SPIR-V from the same sources) on a real
  device under Wine, with core and synchronization validation on (each proved active) and zero errors required: the
  D3D12 suite's frames, sequences and scores (`synth-motion-d3d12/sequences.h`), judged by its `judge()`, a pan and the
  cut also three frames in flight, and GPU time. SKIP when the device cannot run it. No NGX, no game.
- `vulkan-overlay` — builds a real DLL and drives real Vulkan to exercise overlay
  lifetime.
- `vulkan-present-host` — the production DLL from `build-local.sh`, uninstrumented, under a Win32 Vulkan application,
  with core and synchronization validation on (proved active) and zero errors required: with `[DlssNr]
  VulkanPresentHost` on (under the marker, under `SkipHooks`, beside the bundle with its menu off and on) every line
  of the host is held against the application's own record -- which presents the round trip carried, by the same
  queue or across families from a compute-only presenter, with zero, one and two waits, and which it refused when
  more were in flight than its ring holds; usages edited at creation; retirements at recreation, resize and device
  destruction -- and every presented image read back equals the pattern the application made. A surface that hides
  TRANSFER_SRC (a native test layer) keeps every swapchain observe-only. With the key or the master off there is no
  host line, and under the marker vulkan-1.dll's exports are byte-identical to before OptiScaler loaded. Plus a D3D11
  and a D3D12 title through a local Proton's DXVK (validated) and vkd3d-proton: their swapchains get TRANSFER_SRC and
  every present is carried (SKIP without a local Proton). Stage 3 (`nr*` cases, `nr.cpp`), under Proton with the real
  model from the local kit and the driver's NGX core: cold NR on every SDR present (BGRA8 UNORM and sRGB,
  cross-family, an extent not a multiple of eight, working scale 0.5) -- every read-back exact with `ApplyModel` off,
  changed by about three eight-bit steps with it on -- and refused, with every read-back exact, for a device without
  `VK_NVX_binary_import` (the test layer hides it), the kit's old forwarder, and no model (SKIP without the kit or a
  local Proton). Stage 5 (`nr-synth-motion`, `[DlssNr] SynthMotion`): the picture panning, the estimator made with every
  product off the present path and its field the model's motion guide after each swapchain's warm-up, validation
  clean, at native and at half the working size (the field at the model's working size); `nr-pan-zero` is the same pan
  on zero motion, and both report how much of the unmoved previous frame the
  model's edit carries. Stage 4: `nr-trace` holds the pacer trace's counts to the application's, and `nr-fg-vk`
  (`-transport`) runs OptiScaler's Vulkan FSR-FG output with no DLSS-G input: every present set aside as frame
  generation (or carried without NR in the experiment arm), never the model, validation reports only FSR-FG's own. No
  game, no Streamline and no generated frame.

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
