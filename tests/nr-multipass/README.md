# Bounded NR chain host checks

Run `python tests/nr-multipass/run.py` from the repository root. The runner builds in
an automatically removed temporary directory with g++ C++20, ASan and UBSan.
It fails on compiler errors, sanitizer findings or assertion failures.

The production portable chain helpers are executed directly: odd/even final output,
immutable original proxy, partial-failure selection, one observed evaluation per present,
500 ms configuration debounce and creation spacing, adapter-budget admission, retirement
cap, matched native/working resolve pair and creation submitted/completed gating. There
are 20 cases and 1000 repeated same-present refusals. Source checks confirm the renderer
uses those helpers and has exactly one supersampling down-leg using the final answer.

This does not run the D3D12 renderer, the NGX model, shader code, real queue submissions,
real memory-budget accounting or GPU timing. Partial failure here tests selection policy,
not a real NGX failure. The submission suite separately exercises tracker replays and
unknown/failed fence submissions. No host pass is evidence of in-game correctness.

INFO logs `DLSS-NR chain` report requested/completed passes, stage, present, initialization,
headroom refusal and individual evaluation return codes. Per-pass `CPU-call-ms` measures
host time spent inside the forwarder, **not GPU execution**. The existing NGX GPU interval
covers the whole chain; per-pass GPU timings are not available. Stage coverage continues
to count one final composition, rather than calling three model passes three applied frames.

The recording-lease checks cover borrowing only the explicit pre-upscale scope token,
rejecting unrelated scopes even on the same list, and refusing new recordings while the
previous epoch is incomplete or completed but replayable. Only Reset plus fence completion
allows another recording. This intentionally reduces throughput when the game keeps lists
open or the GPU is behind. An A/B must compare applied coverage as well as timings; skipped
NR frames must not be presented as a faster model.
