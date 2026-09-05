# Pre-upscale NR boundary tests

From the repository root:

```sh
python tests/nr-before-upscale/run.py
python tests/nr-before-upscale/verify-invariants.py --base 660303ec
./build-local.sh Release
```

Requires Python and g++ with AddressSanitizer/UndefinedBehaviorSanitizer support.
The runner extracts the **current production** `ScopedPreUpscale` constructor and
destructor, `EvaluateAfterUpscale`, scratch allocation, and sRGB checks; changed or
missing extraction anchors fail. It compiles those functions with strict host fakes
and includes the production color-binding and stabilization helpers directly.

PASS means assertions completed with no sanitizer failure: disabled/default paths do
not read/swap parameters or allocate pre-upscale textures; typed/untyped slots are
restored even if the caller throws; no replacement occurs without a successful
composition; pre/post routing runs at most once; configured input states are restored;
nested scopes do not borrow the same scratch; unsupported contracts are rejected;
1000 failed-allocation attempts cause one allocation; 1000 changing DRS evaluations
cause none. Checks also cover the 500 ms threshold and format-change debounce.

These tests **do not run D3D12, the composition shader, the NVIDIA model, a real
upscaler, or a game**. The fake composition reports success/failure to exercise the
real boundary. GPU completion, delayed retirement, image quality and performance
require separate integration/game validation. The Vulkan overlay harness exercises
none of these NR paths and is not evidence for them.

Generated sources and executables remain in the ignored local `artifacts/` directory.
No solution/project or release packaging file includes this test.

The invariant check is specific to this shader-free port: it requires the C++
constant list, HLSL source and both bytecode headers to match the selected baseline,
checks ordered scalar correspondence, and verifies Stage declaration/read/save/INI.
It does not prove byte-identical rendered frames or run the full Config persistence code.

## INFO coverage for the later game A/B

Use file logging at INFO (`[Log] LogToFile=true`, `LogLevel=2`); debug logging is not
required. Search for `DLSS-NR coverage`. The three independent buckets are `before`,
`after` (Stage 0), and `after-fallback` (Stage 1 declined). Native Vulkan is outside
this D3D12 instrumentation. A normal Stage 1 after-pass stand-down is not counted.

- `calls`: boundary evaluations, **not frames**. `calls = applied_recorded + skipped + fallback`.
- `model_ok` / `model_failed`: model API calls that returned success/failure. Success
  is recorded at the real NGX/proxy return, independently of the resolve result.
- `applied_recorded`: resolve commands were recorded and the boundary selected that
  output (pre: Color slots swapped; post: upscaler output written). This does not
  prove queue submission, GPU completion, upscaler consumption or displayed pixels.
- `skipped`: no selected composition and no fallback request. `fallback` counts a
  pre-stage handoff; look for the separate after-fallback result to see what happened.
- `applied_present_ids` / `skipped_present_ids`: advancing nonzero observed present
  ids in each outcome bucket (fallback is included in skipped-present ids). Repeated
  evaluations on one id count once; zero/backward ids do not count. One present may
  be in both buckets and stages, so **do not sum these as unique game frames**.
- `extent`, `model`, `present_id`, `last`: dimensions, observed present id and reason
  for the evaluation producing this line. Model size is zero if no model call ran.
- `window_*`: deltas since the previous line for this stage, including that line's
  following evaluations up to the current one. `ZERO_APPLIED_IN_WINDOW` means no
  composition was selected in that interval, even if an earlier interval succeeded.

The first entry, first application and first model failure log immediately. Further
entries produce a summary every five seconds, including all-skip runs. This is an
evaluation-driven heartbeat, not a background timer: silence means no coverage
evidence, not success. NR disabled emits no coverage and does no coverage allocation.

A valid performance sample needs sustained `window_model_ok` and `window_applied`
on the intended stage after warm-up, with skipped/fallback proportions reported.
`model_ok > 0` with `applied_recorded = 0` is **not** NR application. Entirely missing
coverage, zero application, or only `after-fallback` in a before-stage run invalidates
that A/B. Use separate processes for Stage 0 and Stage 1; do not include stabilization
intervals as faster model execution. Visual/GPU validation is still separate.

The host suite now extracts the production logging scope and formats INFO lines
using the repository's fmt implementation. It checks zero-application labeling,
per-stage routing, model-success-without-application, failure, disabled silence,
same-present deduplication, and the all-skip five-second heartbeat with a controlled
clock. Model execution remains a fake; the emitted sample line is synthetic evidence.

## State and exposure regression coverage (PR #14/#16 adaptation)

The suite also extracts the production resource-read/restore guards, guide-clone
selection, and exposure SRV validator. It covers typed and typeless guides with
configured arrival states, partial clone failure, explicit restoration followed by
scope exit (no duplicate barrier), and compatible/invalid texture descriptors and
shader-load support. These checks exercise host state transitions, not GPU validation.

Production exposure decisions are included directly from `DlssNr_Exposure.h`:
sources 0/1/2 and unknown sources, HDR/passthrough, missing/current/held exposure,
NaN/Inf/nonpositive/out-of-range readings, trim, anchored fallback, source reentry,
and suppression of live exposure and meter reads while holding color. The existing
`preExposure / exposure * trim` operation order is retained; PR #14's alternative
GameWhite formula and ExposureScale constant are intentionally not imported.

Frame hold now disables live t4 exposure for both encode and resolve, stops the
exposure courier and scan sampling, and uses the captured CPU white point. The first
held frame can therefore use the delayed CPU reading instead of the immediately
preceding live GPU sample; it is a stable comparison, not a pixel-identical capture
of the live shader white point. A failed held-color allocation leaves color live.
Exposure texture validation is conservative: unknown/integer/depth/array/MSAA or
non-loadable views are rejected and the existing CPU fallback is used. Readback
completion and resource retirement are still not proven by this host suite.
