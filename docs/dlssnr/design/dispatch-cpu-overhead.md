# DX12 composition CPU preparation

The composition pass previously mapped and unmapped its upload buffer, queried its GPU address,
and recreated the same constant-buffer descriptor at every dispatch. The buffer and descriptor
slot already lived for the lifetime of `DlssNr_Dx12`.

Each of the existing 48 upload buffers is now mapped once during construction. Its CBV is
initialized after descriptor-heap creation. `DispatchPass` copies the constants into the chosen
slot and continues to bind the same SRV/UAV tables and record the same compute dispatch. Destruction
unmaps each acquired mapping before releasing its buffer, including partially initialized objects.

This follows the [D3D12 persistent Map contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12resource-map):
CPU writes precede submission, the CPU does not read upload memory, and the mapping never outlives
the resource. This does **not** improve or replace synchronization: the 48-slot ring and its existing
retirement/recording policy are unchanged. In particular, legacy round-robin reuse is not a proof
of GPU completion.

The recording admission helpers now consume the same pass-settings snapshot used by their caller.
This removes two repeated snapshot transactions in `Dispatch` and one in the pre-upscale scope.
The dispatch-owned recording lease lives in an optional on the stack, preserving its destructor
order and borrowing semantics without a heap allocation. The model-file identity returns a
reference to its existing process-lifetime string to avoid copies during timing metadata creation.
Timing metadata also reuses the prior contract hash while the exact contract text is unchanged.
The hash is recomputed before publishing each changed contract, preserving generation/log semantics,
including changes back to an earlier configuration. Text formatting/comparison still happen each
instrumented evaluation; this only removes repeated hashing. Dispatch's existing `g_nrMutex`
serializes access to this cache. Failed string assignment cannot publish a hash for an old contract.

## Review against DEVELOPMENT.md

- NR-disabled entry guards still precede construction/dispatch. The optimization is confined to
  NR files; it adds no hooks or work when NR is off.
- Constant-buffer storage remains distinct per slot. Selection, advancement and ring size do not
  change. SRVs/UAVs are still refreshed every dispatch, so changing input resources cannot leave
  stale resource views.
- Initialization becomes successful only after all buffers, mappings, pipeline and heaps exist
  and all CBVs have been written. Failed initialization cannot dispatch. Every successfully
  acquired mapping and buffer has one cleanup path.
- No shader, cbuffer layout, composition parameters, config key, UI control or Vulkan resource
  change. The shader/config/UI/Vulkan-specific review types therefore do not apply.
- Pass admission and scheduling use one snapshot per caller; this does not claim atomicity for
  the legacy scalar settings still read separately by the renderer.

## Validation

- Baseline host run: 8/9 suites passed. `nr-timing-boundary` still expected two mutually exclusive
  first-pass evaluate calls after they had already been consolidated into one. The guard now
  requires one call within each marker pair and two in the full dispatch, retaining detection of
  forwarder calls outside the timing markers.
- Optimized host run: 10/10 suites passed under their existing sanitizer configurations.
- New `nr-dispatch` suite compiles the actual class declaration, constructor, dispatch and destructor
  with strict host fakes. It checks exact constant bytes and slot isolation across 145 passes,
  descriptor fallbacks, dispatch extents, invalid arguments and 148 initialization-failure cases.
  It also asserts one map and CBV per buffer, with no unmap until cleanup.
- Release build through `build-local.sh` passed; compiler-prefix watchdog recovered one stall.
  Existing XeSS inheritance and linker warnings remain. clang-format 20 and whitespace checks pass
  for the changed renderer files.
- Real Proton loopback: the optimized build completed the seven-step resolution sweep (5 creates,
  4268 upscale evaluations), with NR composition logs for 3440x1440 and 2560x1080. It then failed
  inside `_nvngx.dll` during `NVSDK_NGX_D3D12_Shutdown1`; that historical run failed. The harness README
  records the corrected compiler watchdog, diagnostic logging and menu routing. Local evidence is
  retained under `x64/dispatch-validation/`.
  The immediately preceding DLL (`fb079c16`) completed the same corrected harness with
  4487 evaluations and then crashed at the same `_nvngx.dll` RVA `0x3af44`. The different evaluation
  counts are not a benchmark: the sweep is wall-clock bounded, includes initialization and skips,
  and was run with diagnostic logging.

- A separate proxy ABI correction subsequently passed all 11 host suites and the complete
  real sweep including shutdown, with these composition optimizations still present. See
  `ngx-shutdown-order.md`; local evidence is in `x64/dispatch-validation/core-abi/`.
- The subsequent timing-hash change passes all 16 metadata/UI boundary cases, including returning
  to an earlier configuration and repeated evaluations with an unchanged generation/hash.
- Final Release build `20260905_224708` passes the full Proton sweep with timing disabled
  (6115 evaluations) and enabled (6200 evaluations). Both shut down normally. The enabled run
  confirms GPU samples and emits exactly three contract generations; returning to the original
  extent returns to its original hash. These counts remain coverage evidence, not a benchmark.
  Final logs and hashes are under `x64/dispatch-validation/final-{default,timing}/` and
  `final-manifest.json`.

Reduced API/snapshot/allocation counts are the demonstrated CPU-work reduction. These checks
establish neither an FPS improvement nor image-quality acceptance; those require measured runtime
comparison. The loopback harness exercises the real path separately from these host tests.
