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
| `host` | Python, `g++`, `clang++` | the `nr-*` suites, `bridge-lifetime`, `vulkan-query-readiness`, `mfg-pattern`, `fg-synth-policy` and `sl-log-repeat` |
| `wine` | msvc-wine prefix; `vulkan-overlay` also needs a graphical session and a working Vulkan loader | `nr-gpu-timing-d3d12`, `synth-motion-d3d12`, `vulkan-overlay`, `dlssnr-loopback` |
| `wip` | registered, no runner yet | — |

The wine tier is serial on purpose. These suites compile through the single msvc-wine
prefix, which is also what `build-local.sh` uses, so do not start one while a
build is running. `CLAUDE.md` has the stall signature and the recovery.

## What the suites cover

**Host tier.** Twenty-four suites. All but `nr-invariants` build real production code with local fakes under
AddressSanitizer and UndefinedBehaviorSanitizer.

- `bridge-lifetime` — production D3D11/D3D12 bridge waits, failed submission, resize,
  ownership and deferred release against scripted COM/Win32 fakes, and which FG input and output
  the bridge is built for. Also a refused presenter resize and its recovery, and the D3D12 overlay's
  render-target release under the present-thread race that caused it. No GPU coverage.
- `vulkan-query-readiness` — production Vulkan upscaler timing, nonblocking
  readiness retry, error rejection and single consumption. No frame-identity
  or GPU synchronization coverage.
- `mfg-pattern` — the real MFG byte signatures and scanner matcher against synthetic
  buffers. Runs no patcher and touches no DLL.
- `sl-log-repeat` — the Streamline log repeat filter against the real `setDynamicMFGParams` line
  on a fake clock: one line per 10 s with the folded count, new sites at once, a bounded table that
  fails open. Runs no Streamline and no callback.
- `fg-synth-policy` — synthesized FG's decisions (fast-motion response on incoherent motion
  and its cap, low-fps floor, duplicate-present advice, which HUD fix runs for which output), the
  motion statistics they are made from, and the NR/FG motion handoff, from the production headers,
  plus a guard holding the HUD mask's thresholds equal to DLSS-NR's. Pure logic on rows laid out as
  the readback lays them out: no GPU, no FSR, no DLSS-G. Also the UI layer's own mask on the CPU, the shader's
  header over fixtures: a glyph and a 1 px frame over a pan are taken whole, and a swaying silhouette, a moving
  patch, rain and a straight edge along a pan are not.
- `nr-before-upscale` — pre-upscale boundary functions against strict host fakes.
- `nr-dispatch` — composition constants, descriptor slots and partial initialization cleanup.
- `nr-enlarge` — Transfer 2, the model's edit enlarged by a private DLSS Super Resolution: the
  decision table (what the resolve is sent and why, supersampling and Vulkan included), the carrier
  through FP16, the NGX adapter's parameters and release order against a recording core, and
  production's plan, build, evaluate, retirement and Retry, sliced out of the renderer and run against
  D3D12 fakes that chain every barrier. No GPU, NGX or shader.
- `nr-gpu-timing` — portable timing validation, plus the duplicate-execution
  guard at the submission boundary.
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
- `nr-present-host` — what the production present host builds and when: a frame too small
  for the model, a resize, the pass switched off, an HDR swapchain declined, and on which list the
  model is created.
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
per-change-type review in `OptiScaler/dlssnr/design/DEVELOPMENT.md` §3 before
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
