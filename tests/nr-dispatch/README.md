# NR composition dispatch

Run `python3 tests/nr-dispatch/run.py`. The runner compiles the production class declaration,
constructor, `DispatchPass` and destructor against strict host D3D12 fakes with ASan/UBSan.
The constants layout is the real shared header.

Checks 145 dispatches across the 48-slot ring: exact uploaded bytes, isolation between slots,
dispatch dimensions, optional SRV/UAV stand-ins, invalid inputs, and one map/CBV per buffer
instead of per dispatch. Exercises allocation failure, Map failure and a null mapping at
every slot, plus root/pipeline/heap failure and a null device. Each acquired mapping/resource
must be cleaned up once, and a partially initialized shader must refuse dispatch.

This verifies CPU preparation and cleanup, not GPU execution, descriptor retirement, frame
generation safety, image quality or FPS. The existing ring reuse policy is unchanged.
