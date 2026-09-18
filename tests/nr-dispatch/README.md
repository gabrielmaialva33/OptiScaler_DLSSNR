# NR composition dispatch

Run `python3 tests/nr-dispatch/run.py`. The runner compiles the production class declaration,
constructor, `DispatchPass`, destructor and the scratch generation against strict host D3D12 fakes
with ASan/UBSan. The constants layout is the real shared header.

Checks 145 dispatches across the 48-slot ring: exact uploaded bytes, isolation between slots,
dispatch dimensions, optional SRV/UAV stand-ins, invalid inputs, and one map/CBV per buffer
instead of per dispatch. Exercises allocation failure, Map failure and a null mapping at
every slot, plus root/pipeline/heap failure and a null device. Each acquired mapping/resource
must be cleaned up once, and a partially initialized shader must refuse dispatch.

## Descriptor reuse

`DispatchPass` creates a view only when the key describing it moved, so this suite asserts descriptor
**contents** and not call counts: a cache that skips a write it owed leaves the previous
incarnation's record in the slot, and counting reports that as a saving. `ViewRecord` in `fakes.h`
carries what each slot was given, `translateTypeless` included, and the fake view creators mirror the
real `Shader_Dx12` signatures so a production call that stops passing an argument is recorded as the
default it actually got.

Three cases cover retirement: six reshapes (one per description field the key carries), an advanced
generation, and a saturated one. Two guards in `run.py` cover what no behavioural test can see while
the view arguments are constants — that every argument a call site passes is also in the key, and
that every member of `BindKey` appears in its own `operator==`.

Design note: `OptiScaler/dlssnr/design/descriptor-reuse.md`.

## What it does not cover

CPU preparation and cleanup only: not GPU execution, not whether a reused descriptor is accepted by a
real driver, not frame generation safety, image quality or FPS. Fewer view-creation calls is a count,
not a profile. The existing ring reuse policy is unchanged.
