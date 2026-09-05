# NR state, exposure, multipass and optional localization

Status: implementation in progress on dlss-neural-rendering, starting at b11ed12f.
No game installation or push is authorized by this work.

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

## Review matrix and validation

State restoration: typed/typeless guides, nondefault arrivals, partial clone creation,
proxy/failure exits, output transitions and feature-creation bindings. Exposure: source
0/1/2, HDR/passthrough, absent/invalid/current/held exposure and frozen color. Multipass:
one-pass default, odd/even final surface, partial initialization, failure on later layers,
resolution/settings changes, multiple evaluates per observed frame and incomplete GPU
submissions. Config: declare/read/save/shipped defaults, inheritance and deletion. Menu:
only effective backend controls, one label per quantity, existing white-point table.
Localization: UTF-8, placeholders, hidden IDs, absent file, invalid entries and fallback.

Host tests and Release compile/link are necessary but do not prove real NGX operation,
GPU lifetime, visual quality or performance. Preserve that separation in the final report.
