# Tests

Every suite is a self-contained directory with its own `run.py` and `README.md`.
Nothing here is a unit-test framework: each `run.py` slices production source out
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
| `host` | Python, `g++`, `clang++` | the nine `nr-*` suites below |
| `wine` | msvc-wine prefix; `vulkan-overlay` also needs a graphical session and a working Vulkan loader | `nr-gpu-timing-d3d12`, `vulkan-overlay` |
| `wip` | registered, no runner yet | `dlssnr-loopback` |

The wine tier is serial on purpose. Both suites drive the single msvc-wine
prefix, which is also what `build-local.sh` uses, so do not start one while a
build is running. `CLAUDE.md` has the stall signature and the recovery.

## What the suites cover

**Host tier.** All nine build real production code with local fakes under
AddressSanitizer and UndefinedBehaviorSanitizer.

- `nr-before-upscale` — pre-upscale boundary functions against strict host fakes.
- `nr-gpu-timing` — portable timing validation, plus the duplicate-execution
  guard at the submission boundary.
- `nr-gpu-timing-config` — the Config GPU-timing transactions and their INI
  expressions.
- `nr-localization` — the real portable `.lang` parser and the repository ImGui
  hash. Slowest suite, around 45 seconds.
- `nr-menu` — pass-menu control flow driven by scripted ImGui events.
- `nr-multipass` — portable multi-pass chain helpers.
- `nr-pass-config` — the pass-settings codec and the four-point Config round trip
  for the master keys.
- `nr-submission` — the command-list submission lifetime model that gates
  resource release.
- `nr-timing-boundary` — timing metadata and UI boundary, and where the timing
  markers sit.

**Wine tier.**

- `nr-gpu-timing-d3d12` — real D3D12 timestamp queries in an isolated prefix. It
  never loads NGX and never launches a game.
- `vulkan-overlay` — builds a real DLL and drives real Vulkan to exercise overlay
  lifetime.

**Not a suite.** `nr-before-upscale/verify-invariants.py` is a one-shot evidence
script pinned to baseline commit `660303ec`. `run.py` does not call it and
`run_all.py` does not run it. Invoke it by hand when you want that comparison.

## What none of this covers

The `nr-*` suites exercise decision logic against fakes. They do not execute NGX,
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
