# NR state, exposure, multipass and optional localization

Status: integrated host suites and Release compile/link passed on dlss-neural-rendering,
starting at b11ed12f. The verified working tree includes the recording completion lease.
The verified Release was built from the integrated working tree before the final
review commit. Artifact identity and the later committed-source rebuild are recorded
in a separate local validation manifest. No GPU validation, game installation or
push is part of the completed evidence.

## Sources and ordered scope

1. PR #16 and the prerequisite of PR #23: restore creation-frame bindings, configured
   guide/exposure arrival states and output/clones on every exit. Adapt to both NR
   stages and to our three GPU exposure reads; do not transplant the stale patch.
2. PR #14: extract testable exposure decisions, verify missing/invalid/current/held
   exposure and source switching. Preserve the existing composition and passthrough.
3. PR #23/#26: bounded D3D12 chained model passes, initially at most three, one NGX
   feature/history per pass and an untouched original proxy anchor. Resolve and the
   supersampling down-leg run once against the last successful answer. Default is one.
4. PR #26: sparse per-pass settings with live master inheritance, finite/range-checked
   input and true save/reload/removal tests. Do not change global INI parsers merely to
   support these sections. Missing or auto values inherit; lowering count retains edits.
5. PR #26 localization commit: optional external UTF-8 dictionary, adapted for pt-BR.
   No dictionary means original English strings. Preserve format directives and ImGui
   identity; keep the language pack optional in packaging and documentation.

Sources: https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/16,
https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/14,
https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/23,
https://github.com/Dagherbou/OptiScaler_DLSSNR/pull/26.

## Resource and frame contract

Additional passes must not create and evaluate on the same observed frame/recording.
Dispatch count and queue Signal before ExecuteCommandLists are not completion evidence.
Establish submission-ordered completion tracking before retiring added resources.
Unknown submission, failed Signal and device loss must retain/abandon resources rather
than pretending that 32 evaluations prove completion. Do not silently enable untracked
multi-queue/bridge paths. Record effective passes and refusal reasons at INFO.

Failure of an additional pass keeps the last successful surface, stops further passes
and latches retry. Counts and parameter edits must not provoke per-frame allocation.
Do not infer unlimited VRAM from host texture size; model allocations are opaque.

## Integrated behavior and remaining lifetime limits

The D3D12 chain is bounded to three requested passes, with one/master as the default.
Additional passes have separate feature/history state. A later failure stops the chain
and preserves the last successful answer; the final resolve and optional supersampling
down-leg use that answer once. Count and per-pass edits are debounced rather than
rebuilding on every slider movement. Native Vulkan and the driver proxy remain one
pass with master settings. INFO status and the menu distinguish requested work from
successful passes in the last evaluation.

Tracking is an opt-in contract established before the first NR recording. Enabling
multipass or individual settings after untracked NR has already run requires an
application restart. Retry cannot retroactively observe those recordings. Once
tracking is active, reducing the requested count does not discard its protection.

The new experimental path requires Ready evidence before reuse between recordings:
the relevant command list has successfully Reset and all observed executions have
completed their submission-ordered fences. A completed fence alone does not exclude
command-list replay; Reset alone does not complete GPU work. An unknown recording,
failed Signal, device loss or incomplete evidence must refuse reuse rather than guess.
The renderer integration of this gate passed its host admission and lease tests. This can
serialize NR work, skip evaluations and reduce throughput; it is a correctness
restriction, not a performance improvement. Zero waits or fewer model calls must not
be presented as a faster model.

The default single/master path retains the inherited 32-evaluation retirement rule.
It still has no general proof of GPU completion. This work does not upgrade the entire
legacy NR path, every bridge or every resource owner to a globally fence-safe design.
Opaque model allocations also make VRAM admission estimates heuristic; a bounded
retirement queue limits growth but cannot establish GPU completion by its size.
DRS/recreation stress remains outside any safety claim for the legacy path. The
tracked experimental path also needs real submission/replay/resize integration tests
before such stress can be described as validated.

## Review matrix and evidence recorded so far

| Area | Executed evidence | What remains unproved |
| --- | --- | --- |
| Pass configuration | 19 host cases passed with ASan/UBSan: production codec and extracted Config methods, seven sparse fields, live inheritance, real save/reload/removal, retained inactive passes and 5,000 concurrent transactions; both new master keys have all four config points | Windows compile/link passed; live application reload remains untested and sanitizers used are not a race detector |
| Pass menu | 15 scripted frames passed with ASan/UBSan using actual extracted menu/deferred-slider functions: unsupported backend flags, all fields, inheritance, clear, count changes and retained edits; project/filter registration guard passed | Actual ImGui rendering, layout, font glyphs and live State/backend discovery |
| Localization | Real dictionary parser and ImHashStr passed: 124 supplied pt-BR entries, 110 production labels, four hash seeds and four runtime loader modes (absent, inactive, invalid, active) | Live Windows DLL loading and visual layout |
| Submission lifetime model | Nine host scenarios passed, including 3,000 retirements and 1,000 concurrent Reset/notification pairs; source guard checks notification follows original ExecuteCommandLists in both hooked paths | Real COM identity/refcounts, Detours interception, Proton/D3D12 queue behavior, GPU completion, replay and device loss |
| Shared shader contract | 23 ordered 4-byte constants match HLSL; shader and both compiled headers unchanged; default model/meter/encode/resolve arguments and passthrough flags match the baseline guard | Rendered-pixel identity or GPU numerical behavior |
| State restoration and exposure | 56 host boundary cases passed with ASan/UBSan, including guide states/clones, restoration, exposure source/validity, held-color decisions, spatial diagnostics and 1,000-iteration allocation-failure/DRS loops | Real NGX bindings, GPU barriers, readback completion and held-frame visual equivalence |
| Multipass renderer | 20 production-helper host cases passed with ASan/UBSan, including Ready/lease admission, routing, partial failure, creation gates and 1,000 same-present refusals; renderer source guards passed | Real model chaining, quality, VRAM behavior, GPU safety and performance |
| Release | `./build-local.sh Release` passed with exit 0 under msvc-wine, without `/m`; both DLLs linked and matched their copied `x64/out` outputs | Compile/link is not runtime validation; C4250 and LNK4098 warnings remain |

The integrated rerun used all six `tests/nr-*/run.py` suites and
`tests/nr-before-upscale/verify-invariants.py --base 660303ec`; all passed.
`git diff --check` passed. The baseline argument/shader guard covers unchanged default
model/meter/encode/resolve calls, not every bug-fix behavior: invalid exposure and Hold
handling intentionally correct prior behavior and must not be called byte-identical.

Both output DLLs matched their copied `x64/out` outputs, and the archived b11ed12f
return build retained its recorded hash. Local logs are `/tmp/nr-pr-final-release.log`
and `/tmp/nr-*-integration.log`. C4250 and LNK4098 warnings were also present in the
earlier state-restoration build log; they remain unresolved. A source or build-stamp
change requires a newly identified artifact.

Packaging was checked by source inspection: its explicit files/folders exclude all
`tests/` harnesses; the optional dictionary is copied only to
`translations/optional/pt-BR.lang`, and an active root `OptiScaler.lang` is rejected.
The package script itself was not executed and no release archive was produced.
HLSL and both embedded shader headers are unchanged from b11ed12f and the guard baseline.

The pass-menu adversarial review walked DEVELOPMENT.md section 2 row by row. The
entire Colour/white-point/anchor block remains byte-identical to the menu integration's
starting HEAD: source 0 shows paper white; source 1 keeps exposure trim with supplied
or missing exposure; source 2 without anchors has no trim; with anchors it has scan
trim and paper white only while editing a point; the direction checkbox remains
limited to one anchor. New pass controls have a separate state matrix in
[the menu verification README](../../../tests/nr-menu/README.md).

The two new `[DlssNr]` keys, Passes and IndividualPassSettings, have declaration/default,
read, save and shipped INI entries. Sparse `[DlssNr.PassN]` fields inherit when absent;
all seven are validated, serialized and covered by round trips. The menu uses short
Config setters for master and sparse values and holds no Config lock across renderer
calls. Counts and individual settings are hidden for native Vulkan/proxy; native
Vulkan identification also covers the period before its first NR evaluation.

Translation is deliberately partial. New stable `###` labels can be translated;
legacy bare/`##` widget labels remain English to preserve real ImGui hashes. Supported
section/status/help text translates, and unknown text falls back to English. The
optional pack is inactive in the package and does not select a language by itself.
No complete-menu translation is claimed.

Detailed test boundaries are documented in
[nr-before-upscale](../../../tests/nr-before-upscale/README.md),
[nr-pass-config](../../../tests/nr-pass-config/README.md),
[nr-submission](../../../tests/nr-submission/README.md), and
[nr-localization](../../../tests/nr-localization/README.md).
Host tests and Release compile/link cannot establish real NGX execution, displayed
pixels, visual quality or GPU lifetime. No test above exercises COM/Detours/Proton on
a real D3D12 submission path.

## GTA V b11ed12f observation and the next Quality A/B

The user reported two GTA V runs using b11ed12f:

| Run | Stage | Calls | Model success | Model failure | Applied recorded | Skipped | Fallback | Extent / model |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| A | after | 21,124 | 21,121 | 0 | 21,121 | 3 | 0 | 3440x1440 / 3440x1440 |
| B | before | 12,674 | 12,664 | 0 | 12,664 | 10 | 0 | 3440x1440 / 3440x1440 |

The report contained no exceptions or DEVICE_LOST, and one initial
ZERO_APPLIED_IN_WINDOW per run while the model was being created. This supports
successful stage routing and stability during those sessions. Recorded application
means the boundary selected recorded composition commands; it is not independent
proof of GPU completion, upscaler consumption or displayed-pixel identity.

Both stages processed the same model dimensions, so this comparison cannot show a
benefit from reducing the model's pixel count. DLAA is a plausible explanation, not
the only explanation established by those numbers: full-resolution color allocation,
missing/equal render-subrect metadata, scaling settings or the selected input path
must also be distinguished. A game menu saying Quality alone is insufficient evidence.
The number of calls also cannot compare performance without matched durations and
coverage; dispatch counts are not unique frames.

The next experiment should keep these new multipass features disabled (one/master)
so it isolates stage placement. Use separate processes and the same build, scene,
camera route, output resolution, DLSS Quality selection, NR working scale and tuning.
Keep DRS and Frame Generation off for the initial controlled comparison, with other
frame caps, synchronization and latency settings unchanged. Exclude model creation
and stabilization from the measured interval.

Before accepting any timing sample, establish all of the following in the log:

- The game's effective render subrect is smaller than output, not just a Quality label.
  Distinguish color allocation size from valid subrect size and report the source of
  the dimensions actually used by the pre-stage.
- With NR working scale fixed at 1.0, the before-stage model uses that smaller extent;
  the after-stage model uses output extent. If this does not occur, investigate routing
  or metadata before interpreting performance.
- Sustained model success and recorded application occur in the intended stage in
  both runs. Report per-window skips, fallback, failures and observed-present coverage;
  exclude sustained ZERO_APPLIED_IN_WINDOW intervals. Present buckets can overlap and
  must not be summed as unique game frames.
- Capture external frame-time distributions (for example, MangoHud), including median
  and slow-tail values, matched capture durations and the same repeated scene segment.
  Pair these with admission/application coverage. Do not compare FPS alone or count
  skipped NR evaluations as speedups.
- Treat the current `DLSS-NR cost` GPU timing as an auxiliary diagnostic. The inherited
  `GpuTime_Dx12::ReadGpuTime` reads a previous ring slot without a completion fence and
  uses a queue hint; timestamps may be stale or not paired with the evaluated interval.
  This scope does not change the global timer. Conclusive attribution to NR/model/pass
  cost requires GPU timestamps with verified completion and recording/queue pairing,
  which have not been established by the current host tests.
- Compare motion as well as still images for shimmer, edge instability, detail changes
  and color/exposure consistency. Repeat matched runs to distinguish a stable change
  from scene variability or warm-up.

There is no promised gain: a smaller model extent establishes the intended cost
reduction opportunity; actual total latency, throughput and visual tradeoffs still
require measurement. No game installation or marker change was performed to prepare
this document.
